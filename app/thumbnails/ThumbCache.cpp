#include "ThumbCache.h"

#include <QAtomicInteger>
#include <QBuffer>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QThreadStorage>
#include <QVariant>

namespace ufb {

namespace {

// Resolve (and create) the cache DB path once. Lives under the OS
// cache dir, e.g. ~/Library/Caches/<bundle>/thumbnails.db on macOS
// and %LOCALAPPDATA%\<app>\cache\thumbnails.db on Windows.
QString cacheDbPath() {
    static const QString path = []() -> QString {
        QString dir = QStandardPaths::writableLocation(
            QStandardPaths::CacheLocation);
        if (dir.isEmpty()) {
            dir = QDir::tempPath();
        }
        QDir().mkpath(dir);
        return dir + QStringLiteral("/thumbnails.db");
    }();
    return path;
}

// Per-thread connection holder. QSqlDatabase connections can't be
// used across threads, so each worker thread opens its own named
// connection to the shared file. The holder removes the connection
// when its owning thread exits (QThreadStorage deletes it for us).
struct ConnHolder {
    QString name;
    bool opened = false;
    ~ConnHolder() {
        if (!name.isEmpty() && QSqlDatabase::contains(name)) {
            {
                QSqlDatabase db = QSqlDatabase::database(name, false);
                if (db.isOpen()) db.close();
            }  // db handle out of scope before removeDatabase()
            QSqlDatabase::removeDatabase(name);
        }
    }
};

void ensureSchema(QSqlDatabase& db) {
    QSqlQuery q(db);
    // One row per file. `blob` is an encoded PNG/JPEG master at
    // kThumbMasterSize. (file_size, file_mtime) are the staleness key.
    q.exec(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS thumbs ("
        "  path TEXT PRIMARY KEY,"
        "  file_size INTEGER NOT NULL,"
        "  file_mtime INTEGER NOT NULL,"
        "  width INTEGER NOT NULL,"
        "  height INTEGER NOT NULL,"
        "  blob BLOB NOT NULL,"
        "  data_size INTEGER NOT NULL,"
        "  created INTEGER NOT NULL,"
        "  last_access INTEGER NOT NULL"
        ")"));
    // Eviction orders by last_access; index it.
    q.exec(QStringLiteral(
        "CREATE INDEX IF NOT EXISTS idx_thumbs_lru ON thumbs(last_access)"));
}

// Return the current thread's open connection to the cache DB, or an
// invalid QSqlDatabase if it couldn't be opened. WAL mode + a busy
// timeout let the 4 worker connections interleave without "database
// is locked" errors.
QSqlDatabase threadDb() {
    static QThreadStorage<ConnHolder*> tls;
    static QAtomicInteger<int> counter{0};

    if (!tls.hasLocalData()) {
        tls.setLocalData(new ConnHolder);
    }
    ConnHolder* holder = tls.localData();

    if (holder->opened && QSqlDatabase::contains(holder->name)) {
        return QSqlDatabase::database(holder->name);
    }

    holder->name = QStringLiteral("ufb_thumbcache_%1")
                       .arg(counter.fetchAndAddOrdered(1));
    QSqlDatabase db = QSqlDatabase::addDatabase(
        QStringLiteral("QSQLITE"), holder->name);
    db.setDatabaseName(cacheDbPath());
    if (!db.open()) {
        qWarning("ThumbCache: cannot open %s: %s",
                 qPrintable(cacheDbPath()),
                 qPrintable(db.lastError().text()));
        QSqlDatabase::removeDatabase(holder->name);
        holder->name.clear();
        return QSqlDatabase();
    }
    {
        QSqlQuery pragma(db);
        pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
        pragma.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
        pragma.exec(QStringLiteral("PRAGMA busy_timeout=4000"));
    }
    ensureSchema(db);
    holder->opened = true;
    return db;
}

}  // namespace

ThumbCache& ThumbCache::instance() {
    static ThumbCache c;
    return c;
}

QImage ThumbCache::get(const QString& path, qint64 fileSize, qint64 mtimeMs) {
    if (path.isEmpty()) return QImage();
    QSqlDatabase db = threadDb();
    if (!db.isValid() || !db.isOpen()) return QImage();

    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "SELECT blob FROM thumbs "
        "WHERE path = ? AND file_size = ? AND file_mtime = ?"));
    q.addBindValue(path);
    q.addBindValue(fileSize);
    q.addBindValue(mtimeMs);
    if (!q.exec() || !q.next()) return QImage();

    const QByteArray blob = q.value(0).toByteArray();
    QImage img;
    if (!img.loadFromData(blob)) return QImage();

    // Bump last-access for LRU. Best-effort; ignore failures.
    QSqlQuery upd(db);
    upd.prepare(QStringLiteral(
        "UPDATE thumbs SET last_access = ? WHERE path = ?"));
    upd.addBindValue(QDateTime::currentSecsSinceEpoch());
    upd.addBindValue(path);
    upd.exec();

    return img;
}

void ThumbCache::put(const QString& path, qint64 fileSize, qint64 mtimeMs,
                     const QImage& master) {
    if (path.isEmpty() || master.isNull()) return;
    QSqlDatabase db = threadDb();
    if (!db.isValid() || !db.isOpen()) return;

    // Encode the master. PNG preserves alpha (icons, transparent PDFs);
    // JPEG is far smaller for opaque photographic content.
    QByteArray blob;
    QBuffer buf(&blob);
    buf.open(QIODevice::WriteOnly);
    const bool ok = master.hasAlphaChannel()
        ? master.save(&buf, "PNG")
        : master.save(&buf, "JPEG", 85);
    buf.close();
    if (!ok || blob.isEmpty()) return;

    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSqlQuery q(db);
    q.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO thumbs "
        "(path, file_size, file_mtime, width, height, blob, data_size, created, last_access) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(path);
    q.addBindValue(fileSize);
    q.addBindValue(mtimeMs);
    q.addBindValue(master.width());
    q.addBindValue(master.height());
    q.addBindValue(blob);
    q.addBindValue(blob.size());
    q.addBindValue(now);
    q.addBindValue(now);
    if (!q.exec()) return;

    // LRU eviction: if total bytes exceed the cap, drop the
    // least-recently-accessed rows until back under it. The window
    // function computes a running total newest-first; rows past the
    // cap are deleted. SQLite 3.25+ (bundled with Qt 6) has windows.
    QSqlQuery sum(db);
    if (sum.exec(QStringLiteral("SELECT COALESCE(SUM(data_size),0) FROM thumbs"))
        && sum.next()
        && sum.value(0).toLongLong() > kMaxBytes) {
        QSqlQuery evict(db);
        evict.prepare(QStringLiteral(
            "DELETE FROM thumbs WHERE path IN ("
            "  SELECT path FROM ("
            "    SELECT path, SUM(data_size) OVER "
            "      (ORDER BY last_access DESC, path DESC) AS running"
            "    FROM thumbs"
            "  ) WHERE running > ?"
            ")"));
        evict.addBindValue(kMaxBytes);
        evict.exec();
    }
}

}  // namespace ufb
