#include "net.h"

#include <algorithm>

#include "strings.h"
#include "win.h"
// win.h first: these need the full Windows types.
#include <natupnp.h>
#include <netfw.h>
#include <oleauto.h>

namespace fl::net {

namespace {

bool startsWith(const std::wstring& s, const wchar_t* prefix) { return s.rfind(prefix, 0) == 0; }

int octet(const std::wstring& address, int index) {
    size_t at = 0;
    for (int i = 0; i < index; ++i) {
        at = address.find(L'.', at);
        if (at == std::wstring::npos) return -1;
        ++at;
    }
    return _wtoi(address.c_str() + at);
}

// COM on whatever thread calls us; balanced on scope exit.
struct ComScope {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

template <class T>
struct Ptr {
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

}  // namespace

ShareAddress classify(const sys::NetAddress& a) {
    ShareAddress s{a.address, a.adapter, AddressKind::Other, L""};
    std::wstring lower = toLower(a.adapter);
    int o1 = octet(a.address, 0), o2 = octet(a.address, 1);
    if (lower.find(L"radmin") != std::wstring::npos || o1 == 26) s.vpn = L"Radmin VPN";
    else if (lower.find(L"zerotier") != std::wstring::npos) s.vpn = L"ZeroTier";
    else if (lower.find(L"hamachi") != std::wstring::npos || o1 == 25) s.vpn = L"Hamachi";
    else if (lower.find(L"tailscale") != std::wstring::npos || (o1 == 100 && o2 >= 64 && o2 <= 127)) s.vpn = L"Tailscale";
    if (!s.vpn.empty()) {
        s.kind = AddressKind::Vpn;
    } else if (o1 == 10 || (o1 == 192 && o2 == 168) || (o1 == 172 && o2 >= 16 && o2 <= 31)) {
        s.kind = AddressKind::Lan;
    }
    return s;
}

std::vector<ShareAddress> shareAddresses(const std::vector<sys::NetAddress>& all) {
    std::vector<ShareAddress> out;
    for (const auto& a : all) out.push_back(classify(a));
    std::stable_sort(out.begin(), out.end(),
                     [](const ShareAddress& x, const ShareAddress& y) { return static_cast<int>(x.kind) < static_cast<int>(y.kind); });
    return out;
}

Firewall firewallAllows(const std::wstring& exe, int port) {
    ComScope com;
    Ptr<INetFwMgr> mgr;
    if (FAILED(CoCreateInstance(__uuidof(NetFwMgr), nullptr, CLSCTX_INPROC_SERVER, __uuidof(INetFwMgr),
                                reinterpret_cast<void**>(&mgr))))
        return Firewall::Unknown;
    BSTR image = SysAllocString(exe.c_str());
    VARIANT allowed, restricted;
    VariantInit(&allowed);
    VariantInit(&restricted);
    HRESULT hr = mgr->IsPortAllowed(image, NET_FW_IP_VERSION_ANY, port, nullptr, NET_FW_IP_PROTOCOL_TCP, &allowed, &restricted);
    SysFreeString(image);
    if (FAILED(hr) || allowed.vt != VT_BOOL) return Firewall::Unknown;
    return allowed.boolVal == VARIANT_TRUE ? Firewall::Allowed : Firewall::Blocked;
}

bool addPortMapping(int port, const std::wstring& localAddress, const std::wstring& description, std::wstring& error) {
    ComScope com;
    Ptr<IUPnPNAT> nat;
    if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat)))) {
        error = L"UPnP isn't available on this PC";
        return false;
    }
    Ptr<IStaticPortMappingCollection> maps;
    if (FAILED(nat->get_StaticPortMappingCollection(&maps)) || !maps) {
        error = L"No UPnP router found (or UPnP is turned off on it)";
        return false;
    }
    BSTR proto = SysAllocString(L"TCP"), client = SysAllocString(localAddress.c_str()), desc = SysAllocString(description.c_str());
    Ptr<IStaticPortMapping> mapping;
    HRESULT hr = maps->Add(port, proto, port, client, VARIANT_TRUE, desc, &mapping);
    SysFreeString(proto);
    SysFreeString(client);
    SysFreeString(desc);
    if (FAILED(hr)) {
        error = L"The router refused the port mapping (" + win32ErrorMessage(static_cast<unsigned long>(hr)) + L")";
        return false;
    }
    return true;
}

void removePortMapping(int port) {
    ComScope com;
    Ptr<IUPnPNAT> nat;
    if (FAILED(CoCreateInstance(__uuidof(UPnPNAT), nullptr, CLSCTX_ALL, __uuidof(IUPnPNAT), reinterpret_cast<void**>(&nat))))
        return;
    Ptr<IStaticPortMappingCollection> maps;
    if (FAILED(nat->get_StaticPortMappingCollection(&maps)) || !maps) return;
    BSTR proto = SysAllocString(L"TCP");
    maps->Remove(port, proto);
    SysFreeString(proto);
}

std::wstring userRegion() {
    wchar_t buf[16] = {};
    if (GetUserDefaultGeoName(buf, 16) > 0) {
        std::wstring r = buf;
        if (r.size() == 2 && iswalpha(r[0]) && iswalpha(r[1])) return r;
    }
    return L"";
}

}  // namespace fl::net
