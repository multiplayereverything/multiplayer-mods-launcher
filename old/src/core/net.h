// Network helpers for hosting by direct IP: which of this PC's addresses to
// give friends first, whether Windows Firewall lets them in, and optional UPnP
// port mapping on the router. No UI; UPnP and the firewall query are slow COM
// calls, so run them off the UI thread.
#pragma once
#include <string>
#include <vector>

#include "process.h"

namespace fl::net {

enum class AddressKind { Vpn, Lan, Other };

struct ShareAddress {
    std::wstring address, adapter;
    AddressKind kind = AddressKind::Other;
    std::wstring vpn;  // "Radmin VPN", "ZeroTier", "Hamachi", "Tailscale" when known
};

// What kind of address this is: VPNs friends join through come first, then the
// home network. Radmin VPN uses 26.x, Hamachi 25.x, Tailscale 100.64/10; ZeroTier
// is recognized by its adapter name.
ShareAddress classify(const sys::NetAddress& a);
// The PC's addresses in the order to show them: VPNs, then LAN, then the rest.
std::vector<ShareAddress> shareAddresses(const std::vector<sys::NetAddress>& all);

enum class Firewall { Allowed, Blocked, Unknown };
// Whether Windows Firewall lets other PCs reach `exe` on TCP `port`.
Firewall firewallAllows(const std::wstring& exe, int port);

// Maps TCP `port` on the router to `localAddress` (UPnP IGD). `error` says why
// not (no UPnP router, turned off...). removePortMapping undoes it.
bool addPortMapping(int port, const std::wstring& localAddress, const std::wstring& description, std::wstring& error);
void removePortMapping(int port);

// The user's country as two letters ("NL"), for room lists; empty when unknown.
std::wstring userRegion();

}  // namespace fl::net
