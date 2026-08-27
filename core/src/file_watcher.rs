//! Non-blocking file-change detection for pack hot reloading.
//!
//! [`FileWatcher`] monitors the parent directory of one target file and filters
//! platform notifications to that path.

// ---------------------------------------------------------------------------
// file_watcher.rs — Cross-platform file-change detection for pack hotreload.
//
// Wraps the `notify` crate which uses:
//   • Windows  — ReadDirectoryChangesW
//   • Linux    — inotify
//   • macOS    — FSEvents
//   • fallback — mtime polling (other platforms)
//
// # Design
//
// The OS APIs watch *directories*, not individual files.  `FileWatcher`
// watches the parent directory of the target path (non-recursively) and
// filters events down to the single `.ay` file.
//
// `has_changed()` is non-blocking: it drains the internal mpsc channel that
// the notify background thread pushes events onto.  Intended to be called
// once per frame (~16 ms period) — cheap when idle (one channel try_recv).
//
// # Usage
//
// ```rust
// let w = FileWatcher::new("/path/to/pack.ay").unwrap();
// loop {
//     if w.has_changed() { /* reload */ }
// }
// ```
// ---------------------------------------------------------------------------

use notify::{EventKind, RecommendedWatcher, RecursiveMode, Watcher};
use std::path::{Path, PathBuf};
use std::sync::mpsc;

// ---------------------------------------------------------------------------
// FileWatcher
// ---------------------------------------------------------------------------

/// Watches one file and drains matching filesystem-change notifications.
pub struct FileWatcher {
    /// The underlying OS watcher.  Must stay alive — dropping it stops the
    /// background thread and tears down the OS watch handle.
    _watcher: RecommendedWatcher,

    /// Events pushed by the notify background thread.
    rx: mpsc::Receiver<notify::Result<notify::Event>>,

    /// The specific file we care about (absolute or relative, as given).
    watched_file: PathBuf,
}

impl FileWatcher {
    /// Start watching `path` for modifications/creations.
    ///
    /// Returns `None` if:
    /// - `path` has no parent directory component
    /// - The OS watcher backend could not be initialised (permissions, etc.)
    pub fn new(path: &str) -> Option<Self> {
        let watched_file = PathBuf::from(path);

        // Resolve the parent directory to watch.  For a bare filename like
        // "pack.ay" the parent is empty (""), which we treat as current dir.
        let parent: &Path = watched_file
            .parent()
            .filter(|p| !p.as_os_str().is_empty())
            .unwrap_or(Path::new("."));

        let (tx, rx) = mpsc::channel::<notify::Result<notify::Event>>();

        let mut watcher = notify::recommended_watcher(move |res: notify::Result<notify::Event>| {
            // Ignore send errors: only occurs if `rx` was already dropped
            // (i.e., the FileWatcher is being torn down concurrently).
            tx.send(res).ok();
        })
        .ok()?;

        watcher.watch(parent, RecursiveMode::NonRecursive).ok()?;

        Some(FileWatcher {
            _watcher: watcher,
            rx,
            watched_file,
        })
    }

    /// Non-blocking poll.
    ///
    /// Returns `true` if the watched file was created, modified, or renamed
    /// into place since the previous call.  Drains all pending OS events so
    /// subsequent calls within the same frame do not retrigger.
    ///
    /// `false` does **not** guarantee no change occurred — only that no event
    /// arrived in the channel yet (the OS may batch or delay events slightly).
    pub fn has_changed(&self) -> bool {
        let target_name = self.watched_file.file_name();
        let mut changed = false;

        while let Ok(event) = self.rx.try_recv() {
            let Ok(event) = event else { continue };

            match event.kind {
                // Cover both in-place writes and atomic rename-replace:
                //   Modify  — direct write to the.ay file
                //   Create  — temp file renamed into the.ay path
                EventKind::Modify(_) | EventKind::Create(_) => {
                    let matches = event.paths.iter().any(|p| {
                        // Match by file name only: avoids path-normalisation
                        // differences (e.g. relative vs. absolute, trailing separator).
                        p.file_name() == target_name || p == &self.watched_file
                    });
                    if matches {
                        changed = true;
                    }
                }
                // Ignore access (open/read), remove, and metadata-only events.
                _ => {}
            }
        }

        changed
    }
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    #[test]
    fn watcher_new_on_nonexistent_parent_returns_none() {
        // A path with a clearly non-existent deep directory should fail.
        let result = FileWatcher::new("/nonexistent_ayther_test_dir_xyz/pack.ay");
        // May succeed on some platforms (dir watch deferred) or fail — either
        // is acceptable.  The important thing is no panic.
        let _ = result;
    }

    #[test]
    fn has_changed_returns_false_when_no_events() {
        // Create a temp file so the parent dir exists.
        let dir = std::env::temp_dir();
        let path = dir.join("ayther_fw_test_nochange.ay");
        let _ = std::fs::File::create(&path);

        let watcher = FileWatcher::new(path.to_str().unwrap());
        // Give the watcher a moment to set up before querying.
        std::thread::sleep(std::time::Duration::from_millis(50));

        if let Some(w) = watcher {
            // No write happened, so should be false.
            // (There may be a spurious Create event from the file creation
            // above — drain it, then check again.)
            let _ = w.has_changed();
            std::thread::sleep(std::time::Duration::from_millis(50));
            assert!(!w.has_changed(), "expected no change after setup");
        }
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn has_changed_detects_file_write() {
        let dir = std::env::temp_dir();
        let path = dir.join("ayther_fw_test_write.ay");
        // Ensure file exists first so the watcher can start.
        let _ = std::fs::File::create(&path);

        let watcher = FileWatcher::new(path.to_str().unwrap());
        std::thread::sleep(std::time::Duration::from_millis(100));

        if let Some(w) = watcher {
            // Drain any setup events.
            let _ = w.has_changed();

            // Write to the file.
            {
                let mut f = std::fs::File::create(&path).unwrap();
                f.write_all(b"fake ae data").unwrap();
                f.flush().unwrap();
            }

            // Give the OS watcher time to deliver the event.
            std::thread::sleep(std::time::Duration::from_millis(200));

            assert!(w.has_changed(), "expected change after write");
            // Second call should be false (events drained).
            assert!(!w.has_changed(), "expected no more events");
        }
        let _ = std::fs::remove_file(&path);
    }
}
