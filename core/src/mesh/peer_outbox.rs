//! Per-peer outbound tasks.
//!
//! Replaces the single `for peer in peers { ... .await; }` broadcast
//! loops that used to live in the mesh coordinator. Under that model,
//! one slow VPN peer's 5s HTTP timeout stalled the broadcast of every
//! other peer's message — for an N-peer mesh with a dead follower,
//! every outbound event cost up to `5s × (N-1)` of sync-worker time.
//!
//! New model: each peer owns its own long-lived task with a bounded
//! mpsc outbox. The service fan-out is an O(n) non-blocking loop of
//! `try_send` calls; a stuck peer fills its own outbox and never
//! touches anything else.

use super::peer_manager::{PeerEndpoint, PeerManager};
use std::collections::HashMap;
use std::sync::Arc;
use tokio::sync::mpsc;

const OUTBOX_CAPACITY: usize = 64;
const PER_REQUEST_TIMEOUT_SECS: u64 = 5;
const MAX_DELIVERY_ATTEMPTS: u32 = 3;

/// One outbound payload shape per wire endpoint we POST to.
#[derive(Debug, Clone)]
pub enum OutboundMsg {
    MetadataEdit {
        job_path: String,
        item_path: String,
        metadata_json: String,
        folder_name: String,
        is_tracked: bool,
        modified_time: i64,
    },
    TableChange {
        change_json: String,
    },
    SnapshotNotify,
}

/// Handle to a single peer's outbox.
pub struct PeerOutbox {
    node_id: String,
    tx: mpsc::Sender<OutboundMsg>,
    task: Option<tokio::task::JoinHandle<()>>,
}

impl PeerOutbox {
    pub fn node_id(&self) -> &str {
        &self.node_id
    }

    pub fn try_send(
        &self,
        msg: OutboundMsg,
    ) -> Result<(), mpsc::error::TrySendError<OutboundMsg>> {
        self.tx.try_send(msg)
    }

    pub async fn shutdown(&mut self) {
        if let Some(task) = self.task.take() {
            drop(std::mem::replace(
                &mut self.tx,
                mpsc::channel::<OutboundMsg>(1).0,
            ));
            let _ = task.await;
        }
    }
}

pub struct PeerOutboxes {
    map: HashMap<String, PeerOutbox>,
    client: reqwest::Client,
    peer_manager: Arc<PeerManager>,
}

impl PeerOutboxes {
    pub fn new(client: reqwest::Client, peer_manager: Arc<PeerManager>) -> Self {
        Self {
            map: HashMap::new(),
            client,
            peer_manager,
        }
    }

    pub async fn add(&mut self, endpoint: PeerEndpoint) {
        let node_id = endpoint.node_id.clone();

        if let Some(existing) = self.map.get(&node_id) {
            let _ = existing;
            return;
        }

        let (tx, rx) = mpsc::channel::<OutboundMsg>(OUTBOX_CAPACITY);
        let client = self.client.clone();
        let ep_for_task = endpoint.clone();
        let node_for_log = node_id.clone();
        let pm = self.peer_manager.clone();
        let task = tokio::spawn(async move {
            peer_task(ep_for_task, client, rx, pm).await;
            log::debug!("Peer outbox task exited for {}", node_for_log);
        });

        self.map.insert(
            node_id.clone(),
            PeerOutbox {
                node_id,
                tx,
                task: Some(task),
            },
        );
    }

    pub async fn remove(&mut self, node_id: &str) {
        if let Some(mut outbox) = self.map.remove(node_id) {
            outbox.shutdown().await;
        }
    }

    pub fn broadcast(&self, msg: OutboundMsg) {
        for (node_id, outbox) in &self.map {
            match outbox.try_send(msg.clone()) {
                Ok(()) => {}
                Err(mpsc::error::TrySendError::Full(_)) => {
                    log::warn!("Peer {} outbox full, dropping message", node_id);
                }
                Err(mpsc::error::TrySendError::Closed(_)) => {
                    log::warn!("Peer {} outbox closed, dropping message", node_id);
                }
            }
        }
    }

    pub fn send_to(
        &self,
        node_id: &str,
        msg: OutboundMsg,
    ) -> Result<(), mpsc::error::TrySendError<OutboundMsg>> {
        match self.map.get(node_id) {
            Some(outbox) => outbox.try_send(msg),
            None => Err(mpsc::error::TrySendError::Closed(msg)),
        }
    }

    pub fn has(&self, node_id: &str) -> bool {
        self.map.contains_key(node_id)
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub async fn shutdown_all(&mut self) {
        let ids: Vec<String> = self.map.keys().cloned().collect();
        for id in ids {
            if let Some(mut ob) = self.map.remove(&id) {
                ob.shutdown().await;
            }
        }
    }
}

async fn peer_task(
    endpoint: PeerEndpoint,
    client: reqwest::Client,
    mut rx: mpsc::Receiver<OutboundMsg>,
    peer_manager: Arc<PeerManager>,
) {
    log::info!(
        "Peer outbox task started for {} @ {}:{} (data_port={})",
        endpoint.node_id,
        endpoint.ip,
        endpoint.port,
        endpoint.data_port
    );

    while let Some(msg) = rx.recv().await {
        let mut attempt: u32 = 0;
        loop {
            attempt += 1;
            let result = dispatch(&client, &endpoint, &msg).await;
            if result {
                break;
            }
            if attempt >= MAX_DELIVERY_ATTEMPTS {
                log::warn!(
                    "Peer {} dropping message after {} attempts",
                    endpoint.node_id,
                    attempt
                );
                // Note the failure on the shared tracker. The
                // peer-discovery loop checks `distinct_recent_failures`
                // each tick and, if multiple peers are failing, takes
                // it as a "the local network just shifted" signal —
                // re-registers the phonebook and broadcasts a fresh
                // UDP heartbeat without waiting for the next periodic
                // tick.
                peer_manager.note_delivery_failure(&endpoint.node_id);
                break;
            }
            let backoff_ms = match attempt {
                1 => 500,
                2 => 1000,
                _ => 2000,
            };
            tokio::time::sleep(std::time::Duration::from_millis(backoff_ms)).await;
        }
    }
}

async fn dispatch(
    client: &reqwest::Client,
    endpoint: &PeerEndpoint,
    msg: &OutboundMsg,
) -> bool {
    let (path, body): (&'static str, serde_json::Value) = match msg {
        OutboundMsg::MetadataEdit {
            job_path,
            item_path,
            metadata_json,
            folder_name,
            is_tracked,
            modified_time,
        } => (
            "api/metadata/update",
            serde_json::json!({
                "job_path": job_path,
                "item_path": item_path,
                "metadata": metadata_json,
                "folder_name": folder_name,
                "is_tracked": is_tracked,
                "modified_time": modified_time,
                "mesh_epoch": super::MESH_EPOCH,
            }),
        ),
        OutboundMsg::TableChange { change_json } => {
            let mut body: serde_json::Value =
                serde_json::from_str(change_json).unwrap_or_default();
            if let Some(obj) = body.as_object_mut() {
                obj.insert(
                    "mesh_epoch".to_string(),
                    serde_json::Value::from(super::MESH_EPOCH),
                );
            }
            ("api/table/update", body)
        }
        OutboundMsg::SnapshotNotify => ("api/snapshot/notify", serde_json::json!({})),
    };

    let url = format!("http://{}:{}/{}", endpoint.ip, endpoint.data_port, path);
    let req = client.post(&url).json(&body);
    match req
        .timeout(std::time::Duration::from_secs(PER_REQUEST_TIMEOUT_SECS))
        .send()
        .await
    {
        Ok(resp) if resp.status().is_success() => {
            log::debug!("Peer {} delivered {:?}", endpoint.node_id, url);
            true
        }
        Ok(resp) => {
            log::warn!(
                "Peer {} returned {} for {}",
                endpoint.node_id,
                resp.status(),
                url
            );
            false
        }
        Err(e) => {
            let chain = error_chain(&e);
            log::warn!(
                "Peer {} delivery failed to {}: {} (is_connect={}, is_timeout={}, is_request={}, is_body={}, is_decode={})",
                endpoint.node_id,
                url,
                chain,
                e.is_connect(),
                e.is_timeout(),
                e.is_request(),
                e.is_body(),
                e.is_decode(),
            );
            false
        }
    }
}

pub fn reqwest_error_chain(err: &(dyn std::error::Error + 'static)) -> String {
    error_chain(err)
}

fn error_chain(err: &(dyn std::error::Error + 'static)) -> String {
    let mut parts = vec![format!("{}", err)];
    let mut cursor: Option<&(dyn std::error::Error + 'static)> = err.source();
    while let Some(inner) = cursor {
        parts.push(format!("{}", inner));
        cursor = inner.source();
    }
    parts.join(" → ")
}

#[allow(dead_code)]
pub type SharedPeerOutboxes = Arc<tokio::sync::Mutex<PeerOutboxes>>;
