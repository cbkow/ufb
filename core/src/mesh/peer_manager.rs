use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Mutex;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct PeerEndpoint {
    pub node_id: String,
    pub ip: String,
    /// Control-plane port (`/api/status` — liveness, fast reads).
    pub port: u16,
    /// Data-plane port (`/api/metadata/*`, `/api/table/*`, `/api/snapshot/notify`).
    /// Required as of the 0.6.0 wire break; absence indicates a pre-split
    /// (legacy) peer that this node should not talk to.
    #[serde(default)]
    pub data_port: u16,
    /// Mesh epoch (path-identity migration). Old builds advertise no
    /// field → deserializes to 0; a new build peers only with same-epoch
    /// nodes so the two builds never exchange data. See
    /// `crate::mesh::MESH_EPOCH`.
    #[serde(default)]
    pub mesh_epoch: u32,
    pub timestamp_ms: i64,
    #[serde(default)]
    pub tags: Vec<String>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PeerInfo {
    pub node_id: String,
    pub tags: Vec<String>,
    pub endpoint: PeerEndpoint,
    pub is_alive: bool,
    pub is_leader: bool,
    pub failed_polls: i32,
    pub last_seen_ms: i64,
    pub has_udp_contact: bool,
    pub last_udp_contact_ms: i64,
}

/// HTTP status response from a peer's GET /api/status
#[derive(Debug, Deserialize)]
pub struct PeerStatusResponse {
    pub node_id: String,
    #[serde(default)]
    pub is_leader: bool,
    #[serde(default)]
    pub tags: Vec<String>,
    #[serde(default)]
    pub enabled: bool,
    /// Mesh epoch advertised on `/api/status`. Old builds omit it → 0.
    #[serde(default)]
    pub mesh_epoch: u32,
}

pub struct PeerManager {
    farm_path: String,
    node_id: String,
    /// Control-plane port published in `endpoint.json`.
    port: u16,
    /// Data-plane port published in `endpoint.json`.
    data_port: u16,
    tags: Vec<String>,
    peers: Mutex<HashMap<String, PeerInfo>>,
    is_leader: Mutex<bool>,
    leader_id: Mutex<Option<String>>,
    leader_endpoint: Mutex<Option<PeerEndpoint>>,
    /// Callback invoked when leadership changes. Parameter: am_leader.
    on_leadership_changed: Mutex<Option<Box<dyn Fn(bool) + Send>>>,
    /// Map of peer_id → last delivery-failure timestamp (ms since epoch).
    /// Populated by `peer_outbox` whenever an outbound HTTP request to
    /// that peer fails. Read by the peer-discovery loop to decide
    /// whether to force a phonebook refresh + UDP heartbeat: if ≥2
    /// distinct peers have failed within the recent window, it's
    /// likely OUR side (e.g., NIC just changed) rather than any one
    /// peer being down. Single-peer failures don't trigger anything —
    /// that's the peer's problem, not ours.
    delivery_failure_log: Mutex<HashMap<String, i64>>,
}

impl PeerManager {
    pub fn new(
        farm_path: String,
        node_id: String,
        port: u16,
        data_port: u16,
        tags: Vec<String>,
    ) -> Self {
        Self {
            farm_path,
            node_id,
            port,
            data_port,
            tags,
            peers: Mutex::new(HashMap::new()),
            is_leader: Mutex::new(false),
            leader_id: Mutex::new(None),
            leader_endpoint: Mutex::new(None),
            on_leadership_changed: Mutex::new(None),
            delivery_failure_log: Mutex::new(HashMap::new()),
        }
    }

    /// Record an outbound delivery failure to `peer_id`. Called by
    /// `peer_outbox` after each failed HTTP attempt (post-retry, when
    /// the message is being dropped). Stores the latest timestamp;
    /// older entries are filtered out by `distinct_recent_failures`.
    pub fn note_delivery_failure(&self, peer_id: &str) {
        let now = crate::utils::current_time_ms();
        let mut log = self.delivery_failure_log.lock().unwrap();
        log.insert(peer_id.to_string(), now);
    }

    /// Number of distinct peers that have had a delivery failure
    /// within the last `window_ms` milliseconds. Used by the
    /// peer-discovery loop to decide whether the local network has
    /// likely just shifted under us (≥2 peers all simultaneously
    /// unreachable is a strong "our NIC changed" signal).
    pub fn distinct_recent_failures(&self, window_ms: i64) -> usize {
        let now = crate::utils::current_time_ms();
        let log = self.delivery_failure_log.lock().unwrap();
        log.values().filter(|&&ts| now - ts < window_ms).count()
    }

    /// Drop all tracked delivery failures. Called after the
    /// peer-discovery loop has reacted to a burst (re-registered the
    /// phonebook, sent a fresh UDP heartbeat) so the same burst
    /// doesn't keep re-triggering the response on subsequent ticks.
    pub fn clear_delivery_failures(&self) {
        self.delivery_failure_log.lock().unwrap().clear();
    }

    pub fn node_id(&self) -> &str {
        &self.node_id
    }

    pub fn port(&self) -> u16 {
        self.port
    }

    pub fn data_port(&self) -> u16 {
        self.data_port
    }

    pub fn tags(&self) -> &[String] {
        &self.tags
    }

    pub fn is_leader(&self) -> bool {
        *self.is_leader.lock().unwrap()
    }

    pub fn get_current_leader_id(&self) -> Option<String> {
        self.leader_id.lock().unwrap().clone()
    }

    /// Get the leader's endpoint (for followers to POST edits to).
    pub fn get_leader_endpoint(&self) -> Option<PeerEndpoint> {
        self.leader_endpoint.lock().unwrap().clone()
    }

    pub fn get_peers(&self) -> Vec<PeerInfo> {
        self.peers.lock().unwrap().values().cloned().collect()
    }

    pub fn get_alive_peers(&self) -> Vec<PeerInfo> {
        self.peers
            .lock()
            .unwrap()
            .values()
            .filter(|p| p.is_alive)
            .cloned()
            .collect()
    }

    pub fn get_peer_count(&self) -> usize {
        self.peers.lock().unwrap().len()
    }

    pub fn set_on_leadership_changed<F: Fn(bool) + Send + 'static>(&self, f: F) {
        *self.on_leadership_changed.lock().unwrap() = Some(Box::new(f));
    }

    /// Get the phonebook directory: {farm_path}/v5/nodes/
    fn phonebook_dir(&self) -> PathBuf {
        super::farm_version_root(&self.farm_path).join("nodes")
    }

    /// Write this node's endpoint to the phonebook (includes tags).
    pub fn register_endpoint(&self) -> Result<(), String> {
        let dir = self.phonebook_dir().join(&self.node_id);
        std::fs::create_dir_all(&dir)
            .map_err(|e| format!("Failed to create node dir: {}", e))?;

        let resolved_ip = get_local_ip_for_farm(&self.farm_path);

        let endpoint = PeerEndpoint {
            node_id: self.node_id.clone(),
            ip: resolved_ip,
            port: self.port,
            data_port: self.data_port,
            mesh_epoch: super::MESH_EPOCH,
            timestamp_ms: crate::utils::current_time_ms(),
            tags: self.tags.clone(),
        };

        let json = serde_json::to_string_pretty(&endpoint)
            .map_err(|e| format!("Failed to serialize endpoint: {}", e))?;

        // Atomic write: stage to .tmp then rename. Plain std::fs::write
        // truncates-then-streams, which leaves a partial/corrupt file on SMB
        // over high-latency or flaky links (e.g. VPN) if any packets are lost
        // mid-write. The rename is a single metadata op, so peers either see
        // the previous valid content or the new valid content — never a torn
        // intermediate state.
        let final_path = dir.join("endpoint.json");
        let tmp_path = dir.join("endpoint.json.tmp");
        std::fs::write(&tmp_path, &json)
            .map_err(|e| format!("Failed to write endpoint tmp: {}", e))?;
        if let Err(e) = std::fs::rename(&tmp_path, &final_path) {
            let _ = std::fs::remove_file(&tmp_path);
            return Err(format!("Failed to rename endpoint: {}", e));
        }
        Ok(())
    }

    /// Remove this node's endpoint from the phonebook.
    pub fn unregister_endpoint(&self) {
        let path = self
            .phonebook_dir()
            .join(&self.node_id)
            .join("endpoint.json");
        let _ = std::fs::remove_file(&path);
    }

    /// Discover peers from the phonebook directory.
    pub fn discover_peers(&self) -> Result<Vec<PeerEndpoint>, String> {
        let nodes_dir = self.phonebook_dir();
        if !nodes_dir.exists() {
            return Ok(vec![]);
        }

        let mut endpoints = Vec::new();
        let entries = std::fs::read_dir(&nodes_dir)
            .map_err(|e| format!("Failed to read nodes dir: {}", e))?;

        let now = crate::utils::current_time_ms();
        const FRESH_THRESHOLD_MS: i64 = 15_000;

        for entry in entries.flatten() {
            if !entry.path().is_dir() {
                continue;
            }
            let endpoint_file = entry.path().join("endpoint.json");
            if let Ok(content) = std::fs::read_to_string(&endpoint_file) {
                if let Ok(ep) = serde_json::from_str::<PeerEndpoint>(&content) {
                    if ep.node_id == self.node_id {
                        continue;
                    }
                    if ep.data_port == 0 {
                        continue;
                    }
                    if ep.mesh_epoch != super::MESH_EPOCH {
                        // Pre-identity-migration peer (old build, or a
                        // future epoch). New and old builds form
                        // separate meshes — skip it entirely.
                        continue;
                    }
                    let is_fresh = (now - ep.timestamp_ms) < FRESH_THRESHOLD_MS;
                    let mut peers = self.peers.lock().unwrap();
                    let peer = peers.entry(ep.node_id.clone()).or_insert_with(|| {
                        if !is_fresh {
                            log::info!(
                                "Discovered peer {} with stale endpoint (age={}s), marking not-alive until verified",
                                ep.node_id,
                                (now - ep.timestamp_ms) / 1000
                            );
                        }
                        PeerInfo {
                            node_id: ep.node_id.clone(),
                            tags: ep.tags.clone(),
                            endpoint: ep.clone(),
                            is_alive: is_fresh,
                            is_leader: false,
                            failed_polls: 0,
                            last_seen_ms: if is_fresh { now } else { 0 },
                            has_udp_contact: false,
                            last_udp_contact_ms: 0,
                        }
                    });
                    peer.endpoint = ep.clone();
                    peer.tags = ep.tags.clone();
                    endpoints.push(ep);
                }
            }
        }

        Ok(endpoints)
    }

    /// Run leader election — 3-tier sort matching C++:
    /// 1. has "leader" tag → first (desc)
    /// 2. has "noleader" tag → last (asc)
    /// 3. alphabetical node_id
    pub fn run_election(&self) {
        let was_leader = *self.is_leader.lock().unwrap();
        let now = crate::utils::current_time_ms();
        const PHONEBOOK_FRESH_MS: i64 = 15_000;

        struct Candidate {
            node_id: String,
            has_leader_tag: bool,
            has_noleader_tag: bool,
            endpoint: Option<PeerEndpoint>,
        }

        let mut candidates = Vec::new();

        {
            let peers = self.peers.lock().unwrap();
            for p in peers.values() {
                let phonebook_fresh = (now - p.endpoint.timestamp_ms) < PHONEBOOK_FRESH_MS;
                if !p.is_alive && !phonebook_fresh {
                    continue;
                }
                candidates.push(Candidate {
                    node_id: p.node_id.clone(),
                    has_leader_tag: p.tags.iter().any(|t| t == "leader"),
                    has_noleader_tag: p.tags.iter().any(|t| t == "noleader"),
                    endpoint: Some(p.endpoint.clone()),
                });
            }
        }

        candidates.push(Candidate {
            node_id: self.node_id.clone(),
            has_leader_tag: self.tags.iter().any(|t| t == "leader"),
            has_noleader_tag: self.tags.iter().any(|t| t == "noleader"),
            endpoint: None,
        });

        candidates.sort_by(|a, b| {
            b.has_leader_tag
                .cmp(&a.has_leader_tag)
                .then_with(|| a.has_noleader_tag.cmp(&b.has_noleader_tag))
                .then_with(|| a.node_id.cmp(&b.node_id))
        });

        let leader = candidates.first().map(|c| c.node_id.clone());
        let am_leader = leader.as_deref() == Some(&self.node_id);

        if !am_leader {
            let ep = candidates.first().and_then(|c| c.endpoint.clone());
            *self.leader_endpoint.lock().unwrap() = ep;
        } else {
            *self.leader_endpoint.lock().unwrap() = None;
        }

        {
            let mut peers = self.peers.lock().unwrap();
            for p in peers.values_mut() {
                p.is_leader = leader.as_deref() == Some(&p.node_id);
            }
        }

        *self.is_leader.lock().unwrap() = am_leader;
        *self.leader_id.lock().unwrap() = leader;

        if was_leader != am_leader {
            log::info!(
                "Leadership changed: {} is now {}",
                self.node_id,
                if am_leader { "LEADER" } else { "FOLLOWER" }
            );
            if let Some(ref cb) = *self.on_leadership_changed.lock().unwrap() {
                cb(am_leader);
            }
        }
    }

    /// Poll peers via HTTP GET /api/status.
    pub async fn poll_peers(&self, client: &reqwest::Client) {
        let now = crate::utils::current_time_ms();

        let peer_list: Vec<(String, PeerEndpoint, bool, bool, i64)> = {
            let peers = self.peers.lock().unwrap();
            peers
                .values()
                .map(|p| {
                    (
                        p.node_id.clone(),
                        p.endpoint.clone(),
                        p.is_alive,
                        p.has_udp_contact,
                        p.last_seen_ms,
                    )
                })
                .collect()
        };

        for (node_id, endpoint, _is_alive, _has_udp, _last_seen) in peer_list {
            let url = format!("http://{}:{}/api/status", endpoint.ip, endpoint.port);
            match client
                .get(&url)
                .timeout(std::time::Duration::from_secs(3))
                .send()
                .await
            {
                Ok(resp) if resp.status().is_success() => {
                    if let Ok(status) = resp.json::<PeerStatusResponse>().await {
                        if status.mesh_epoch != super::MESH_EPOCH {
                            // Peer is on a different mesh epoch — drop it
                            // from the map so it never enters election or
                            // gets an outbox.
                            log::info!(
                                "Peer {} on mesh epoch {} (we are {}) — dropping",
                                node_id,
                                status.mesh_epoch,
                                super::MESH_EPOCH
                            );
                            self.peers.lock().unwrap().remove(&node_id);
                            continue;
                        }
                        let mut peers = self.peers.lock().unwrap();
                        if let Some(p) = peers.get_mut(&node_id) {
                            if !p.is_alive {
                                log::info!("Peer {} verified alive via HTTP poll", node_id);
                            }
                            p.is_alive = true;
                            p.failed_polls = 0;
                            p.last_seen_ms = now;
                            p.tags = status.tags;
                        }
                    }
                }
                _ => {
                    let mut peers = self.peers.lock().unwrap();
                    if let Some(p) = peers.get_mut(&node_id) {
                        if p.is_alive {
                            p.failed_polls += 1;
                            if p.failed_polls >= super::PEER_DEAD_POLL_COUNT {
                                p.is_alive = false;
                                log::info!(
                                    "Peer {} marked dead (failed_polls={})",
                                    node_id,
                                    p.failed_polls
                                );
                            }
                        }
                    }
                }
            }
        }

        {
            let mut peers = self.peers.lock().unwrap();
            for p in peers.values_mut() {
                if p.has_udp_contact
                    && (now - p.last_udp_contact_ms) > super::UDP_SILENCE_THRESHOLD_MS as i64
                {
                    p.has_udp_contact = false;
                }
            }
        }
    }

    /// Process a UDP heartbeat from a peer (with tags).
    pub fn process_heartbeat(
        &self,
        node_id: &str,
        ip: &str,
        port: u16,
        data_port: u16,
        mesh_epoch: u32,
        tags: &[String],
    ) {
        if data_port == 0 {
            return;
        }
        if mesh_epoch != super::MESH_EPOCH {
            // Heartbeat from a different mesh epoch — ignore so the peer
            // never enters the map / leader election.
            return;
        }
        let now = crate::utils::current_time_ms();
        let mut peers = self.peers.lock().unwrap();
        let peer = peers.entry(node_id.to_string()).or_insert_with(|| PeerInfo {
            node_id: node_id.to_string(),
            tags: tags.to_vec(),
            endpoint: PeerEndpoint {
                node_id: node_id.to_string(),
                ip: ip.to_string(),
                port,
                data_port,
                mesh_epoch,
                timestamp_ms: now,
                tags: tags.to_vec(),
            },
            is_alive: true,
            is_leader: false,
            failed_polls: 0,
            last_seen_ms: now,
            has_udp_contact: true,
            last_udp_contact_ms: now,
        });
        peer.last_seen_ms = now;
        peer.has_udp_contact = true;
        peer.last_udp_contact_ms = now;
        peer.is_alive = true;
        peer.failed_polls = 0;
        peer.endpoint.ip = ip.to_string();
        peer.endpoint.port = port;
        peer.endpoint.data_port = data_port;
        peer.tags = tags.to_vec();
        peer.endpoint.tags = tags.to_vec();
    }

    /// Mark a peer as dead (not removed — matching C++) and trigger election.
    pub fn process_goodbye(&self, node_id: &str) {
        {
            let mut peers = self.peers.lock().unwrap();
            if let Some(p) = peers.get_mut(node_id) {
                p.is_alive = false;
                p.has_udp_contact = false;
            }
        }
        self.run_election();
    }

    /// Clean up stale peers.
    pub fn cleanup_stale_peers(&self) {
        let now = crate::utils::current_time_ms();
        const STALE_ENDPOINT_MS: i64 = 3600 * 1000; // 1 hour

        let mut peers = self.peers.lock().unwrap();
        let phonebook = self.phonebook_dir();
        peers.retain(|node_id, p| {
            if !p.is_alive {
                let ep_path = phonebook.join(node_id).join("endpoint.json");
                if !ep_path.exists() {
                    log::info!("Removing stale peer {} (no endpoint.json)", node_id);
                    return false;
                }
                if (now - p.endpoint.timestamp_ms) > STALE_ENDPOINT_MS {
                    log::info!(
                        "Removing dead peer {} and deleting stale endpoint.json (age={}m)",
                        node_id,
                        (now - p.endpoint.timestamp_ms) / 60_000
                    );
                    let _ = std::fs::remove_file(&ep_path);
                    let _ = std::fs::remove_dir(phonebook.join(node_id));
                    return false;
                }
            }
            true
        });
    }
}

/// Get local IP address using the UDP socket trick.
/// Falls back to 8.8.8.8 (internet-facing interface).
pub fn get_local_ip() -> String {
    get_local_ip_for_target("8.8.8.8")
}

/// Get the local IP that can reach the farm share.
pub fn get_local_ip_for_farm(farm_path: &str) -> String {
    if let Some(nas_ip) = resolve_mount_host(farm_path) {
        let ip = get_local_ip_for_target(&nas_ip);
        if ip != "127.0.0.1" {
            return ip;
        }
        log::warn!(
            "Farm host {} resolved but local IP is loopback, using fallback",
            nas_ip
        );
    }
    get_local_ip()
}

/// Windows: extract host from a UNC path like `\\192.168.1.50\share`.
#[cfg(windows)]
fn resolve_mount_host(farm_path: &str) -> Option<String> {
    let normalized = farm_path.replace('/', "\\");
    let stripped = normalized.trim_start_matches('\\');
    let host = stripped.split('\\').next().unwrap_or("");
    if host.is_empty() {
        None
    } else {
        Some(host.to_string())
    }
}

/// macOS: extract the NAS host IP from a mount point by parsing `mount` output.
#[cfg(target_os = "macos")]
fn resolve_mount_host(farm_path: &str) -> Option<String> {
    let real_path = std::fs::canonicalize(farm_path).ok()?;
    let real_str = real_path.to_string_lossy();

    let output = std::process::Command::new("mount").output().ok()?;
    if !output.status.success() {
        return None;
    }

    let mount_output = String::from_utf8_lossy(&output.stdout);
    for line in mount_output.lines() {
        if let Some(on_pos) = line.find(" on ") {
            let mount_point = line[on_pos + 4..].split(' ').next().unwrap_or("");
            if mount_point.len() > 1 && real_str.starts_with(mount_point) {
                let source = &line[..on_pos];
                let stripped = source.trim_start_matches('/');
                if let Some(at_pos) = stripped.find('@') {
                    let after_at = &stripped[at_pos + 1..];
                    let host = after_at.split('/').next().unwrap_or("");
                    if !host.is_empty() {
                        return Some(host.to_string());
                    }
                } else {
                    let host = stripped.split('/').next().unwrap_or("");
                    if !host.is_empty() {
                        return Some(host.to_string());
                    }
                }
            }
        }
    }
    None
}

/// Get local IP by routing toward a specific target IP.
pub fn get_local_ip_for_target(target: &str) -> String {
    let target_addr = format!("{}:80", target);
    match std::net::UdpSocket::bind("0.0.0.0:0") {
        Ok(sock) => {
            if sock.connect(&target_addr).is_ok() {
                if let Ok(addr) = sock.local_addr() {
                    return addr.ip().to_string();
                }
            }
            "127.0.0.1".to_string()
        }
        Err(_) => "127.0.0.1".to_string(),
    }
}
