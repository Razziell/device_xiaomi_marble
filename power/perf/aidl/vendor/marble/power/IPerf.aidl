// SPDX-License-Identifier: Apache-2.0
package vendor.marble.power;
import vendor.marble.power.IPerfSession;
// Private vendor-to-vendor API. Not VINTF; no framework/app clients.
interface IPerf {
    // Each returned Binder owns its locks. Last remote reference releases them.
    IPerfSession openSession();
}
