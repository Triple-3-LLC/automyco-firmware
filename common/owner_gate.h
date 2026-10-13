#pragma once
// Who may make a protected change that isn't an HTTP request the web guard can check: UV-C on, Remote
// Pairing Code (common/terrashroom-core.yaml, common/remote-access.yaml). The same switch is reached from
// the web API, Home Assistant's native API and the chamber's own automations, so the decision lives on the
// entity, not in the transport.
//
//   present()            someone is at the chamber: the rear-button Pair window is open (hold 2-4 s)
//   paired_or_present()  a phone has paired (the control key is set, so web_guard already demanded a signed
//                        X-AutoMyco-Key for the request), or someone is at the chamber
//
// The public build (terrashroom-public.yaml) sets both hooks at boot. The owner's build sets neither, so
// there everything is "present": its LAN and keys are the owner's own, as before.
namespace owner_gate {

inline bool (*presence)() = nullptr;  // the rear-button Pair window is open; nullptr = always (owner's build)
inline bool (*paired)() = nullptr;    // a phone has paired (control key set); nullptr = never

inline bool present() { return presence == nullptr || presence(); }
inline bool paired_or_present() { return (paired != nullptr && paired()) || present(); }

}  // namespace owner_gate
