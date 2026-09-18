// SPDX-License-Identifier: Apache-2.0
package vendor.marble.power;
interface IPerfSession {
    // All methods return -ESTALE after main-gate owner revocation. This is a
    // terminal session error, distinct from per-request -EINVAL/-ENOTSUP.
    // Clients must drop the session/aliases and back off. Only a later fresh
    // caller request may open a new session; never replay old locks.
    // 0 creates a lock; positive handle updates one owned by this session.
    // Returns a positive handle or negative errno. Unknown requests fail closed.
    int acquire(int handle, int durationMs, in int[] resources);
    // Returns 0 on release or negative errno (never a fabricated success handle).
    int release(int handle);
    // Appended private method; no resource-array encoding for CHI hints.
    // Only finite 0x1337..0x1339, type=0, camera SID + session UID/PID.
    int acquireHint(int handle, int durationMs, int hintId, int hintType);
}
