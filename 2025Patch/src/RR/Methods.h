#pragma once
#include "../Utils/globals.h"

// All RVAs below verified against the user's DUMPER-7 runtime dump (Aug 2026). Game-code offsets
// happen to be IDENTICAL to the July 19 2025 reference build the original 2025Patch targeted -
// that's why we can reuse the same numbers unchanged. If a future build shifts these, re-dump and
// update this file (or fall back to pattern-scanning).

namespace RR::Methods::Referee {
	// Referee's 4 anti-tamper check functions. Original July 19 offsets.
	uintptr_t Check1 = 0x76E850;
	uintptr_t Check2 = 0x2E6F680;
	uintptr_t Check3 = 0x2E6F670;
	uintptr_t Check4 = 0x2E6F6E0;
}

namespace RR::Methods::LegacyTlsAuthentication {
	// NOP'd -> game accepts any TLS cert (needed for the private server's self-signed cert).
	uintptr_t NotifyServerCertificate = 0x77E5E30;
}

namespace RR::Methods::HTTPManager {
	// HTTPManager.SendRequest(HTTPRequest* request) - the single-arg internal SendRequest that
	// carries the URI on request+0x10. Hooked to redirect ns.rec.net -> CAMPUS_NAMESERVER_HOST.
	uintptr_t SendRequest = 0x77E0950;
}

// RecNet HTTP response callback. CBACIMLIBPF.CLCAHKCJDLC(request, response, promise) fires
// once BestHTTP has a completed response for a RecNet-managed request. Signature is
// (HTTPRequest*, DPEENGNNEEB*, Promise*). The response has the HTTP status code as int32 at
// +0x10 (verified against the runtime dump). Hooking here lets us log "URI [STATUS]" in a
// single line — the URI comes off request+0x10 the same way SendRequest_H reads it.
//
// LIMITATION: only fires for RecNet-managed requests, NOT every HTTP call. Many endpoints
// bypass this callback entirely (Matchmaking/*, /econ/*, etc.). For universal capture, see
// BestHTTP.HTTPResponse.get_DataAsText below.
namespace RR::Methods::RecNetHttp {
	uintptr_t ResponseCallback = 0x7CC2BD0;
}

// BestHTTP.HTTPResponse.get_DataAsText — the "give me the response as decoded text" property
// getter on the BestHTTP HTTPResponse class. Called by any consumer that wants JSON/text
// response bodies. Universal fallback for endpoints RecNet's own callback doesn't handle.
//
// Signature: `Il2CppString* get_DataAsText(HTTPResponse* self)`
//   - StatusCode:   *(int32*)(self + 24)
//   - baseRequest:  *(HTTPRequest**)(self + 120)  — has Uri* at +16
//   - dataAsText:   *(Il2CppString**)(self + 96)  — cached, or freshly built by original call
//
// LIMITATION: some callers consume the raw byte[] and never call this getter. If a filtered
// URL doesn't produce a dump, its consumer is one of those — see Receive below for the fix.
//
// BestHTTP.HTTPResponse.Receive — the LOW-LEVEL body reader. Fires exactly once per response
// once BestHTTP has finished reading the payload. Called BEFORE any consumer touches the
// body, so it dumps regardless of whether anyone asks for get_DataAsText.
//
// Signature: `bool Receive(HTTPResponse* self, int32_t forceReadRawContentLength, bool readPayloadData)`
// Return value: true iff payload was read. After a successful return, `self.Data` (byte[]* at
// +72) and `self.DataLength` (int32 at +80) are populated with the raw response bytes.
namespace RR::Methods::HttpResponse {
	uintptr_t get_DataAsText = 0x77F4F80;
	uintptr_t Receive        = 0x77F42D0;
}

namespace RR::Methods::System::Uri {
	uintptr_t ToString = 0x935AF50;
	uintptr_t ctor     = 0x935C2A0;   // Uri..ctor(string) - single-arg overload
}

// Photon connect entry points. Each takes Photon.Realtime.AppSettings* as its first non-this
// argument; we Prefix them so the game's own AppSettings gets its identity fields (AppId*,
// AppVersion) overwritten to Campus values before Photon actually connects.
namespace RR::Methods::Photon {
	uintptr_t OLPEILEPEAD_JBNCMFDFDLM                 = 0x757E8C0;   // realtime connect
	uintptr_t OEOMKFKDCCG_JBNCMFDFDLM                 = 0x759B710;   // PUN wrapper
	uintptr_t VoiceConnection_ConnectUsingSettings    = 0x75F55D0;   // Photon.Voice
	uintptr_t PhotonVoiceNetwork_ConnectUsingSettings = 0x75FB2B0;   // Photon.Voice.PUN
}

// AppSettings field offsets from Photon.Realtime.AppSettings.h in the dump. Il2Cpp string fields
// are pointers to Il2CppString*; we replace them with pointers returned by il2cpp_string_new.
namespace RR::Methods::AppSettingsFields {
	constexpr ptrdiff_t AppIdRealtime = 0x10;   // Il2CppString*
	constexpr ptrdiff_t AppIdChat     = 0x18;   // Il2CppString*
	constexpr ptrdiff_t AppIdVoice    = 0x20;   // Il2CppString*
	constexpr ptrdiff_t AppVersion    = 0x28;   // Il2CppString*
}

// Resolved at runtime. On protected builds GameAssembly.dll's export table is STRIPPED, so
// GetProcAddress returns null for every il2cpp_* name. When that happens we fall back to a
// UnityPlayer.dll memory scan (see UnityPlayerScan in Patches.h).
namespace RR::Methods::Il2cpp {
	uintptr_t il2cpp_string_new;
	uintptr_t il2cpp_object_get_class;
	uintptr_t il2cpp_object_new;
}

// UnityEngine.Gradient - the managed wrapper around the native gradient. get_colorKeys returns a
// FRESH copy of the color-key array each call; modifying that array locally does not sync back
// unless we then call SetKeys to marshal to the C++ side. Values here are RVAs verified against
// the Aug 2026 DUMPER-7 runtime dump (UnityEngine.CoreModule/UnityEngine/Gradient.h).
namespace RR::Methods::UnityGradient {
	uintptr_t SetKeys       = 0x9987030;   // void SetKeys(GradientColorKey[], GradientAlphaKey[])
	uintptr_t get_colorKeys = 0x9987110;   // GradientColorKey[] get_colorKeys()
	uintptr_t get_alphaKeys = 0x99870D0;   // GradientAlphaKey[] get_alphaKeys()
}

// RRUI.UIGradient - Rec Room's MonoBehaviour that wraps a Unity Gradient for rendering. Hooking
// the setter lets us swap Rec Room's orange 4-stop ramp for the Campus purple/magenta/orange/gold
// ramp on EVERY UI element that ever gets a gradient assigned.
namespace RR::Methods::UIGradient {
	uintptr_t set_Gradient = 0x8BFA6A0;    // void set_Gradient(UnityEngine.Gradient)
}

// RecNet.Runtime.ADOGCFCJLFK - the content-signature verification class.
// Located from DUMPER-7 output (RecNet.Runtime/__NO_NAMESPACE__/ADOGCFCJLFK.h). This is what
// verifies RSA-signed image/CDN responses against the p1.rec.net public key baked into the
// client. Our server can't forge p1.rec.net signatures, so every image response fails verify
// and the client reports an ImageSignature hile (HileType 6) to /api/PlayerReporting/v1/hile.
//
// Two hooks defuse this cleanly:
//   VerifyContentSig  — the "verify body against signatures for this URI" bool method. Force
//                       it to return true, and every signed response is accepted regardless of
//                       what the header actually says.
//   ReportHile        — the "phone home this hile type + message" void method. Nop it so no
//                       hile ever hits the wire even if some OTHER local check trips (avatar,
//                       photon hash, cert mismatch, etc.). Belt-and-braces.
namespace RR::Methods::ContentSig {
	// Currently only the HTTP-level suppressor in SendRequest_H is active - see the
	// comment there for why. This namespace is now RVA reference only; nothing hooks
	// these on this build until we've triaged which are safe on a per-target basis.
	//
	// Do-not-hook table (proved by previous iterations):
	//
	//   Dead (installed OK, NEVER fired - not on the active image-sig code path)
	//     ADOGCFCJLFK.LHBEBEFKBEG          0x7DC3240   bool  (Uri, List<string>, byte[])
	//     ADOGCFCJLFK.NDOMLKEOPEC          0x7DC3690   bool  (byte[], byte[])
	//     ADOGCFCJLFK.GDDJDAMPIFE          0x7DC2900   void  (Uri, List<string>, byte[], byte[])
	//     ADOGCFCJLFK.GHCMBGNDCPK          0x7DC2D50   void  (HileType, string)
	//     COLGOHBMEFO.FOJLNEAPFGE          0x1316C90   void  (wrapper, out HileType, out string)
	//     ModerationManager.GAGPNFMBLKG    0x2161B40   void  (HileType, string, Nullable<int32>)
	//     KOCCAIFFNNK.DAJPJJEBLEB          0x7DDFE00   void  ()  instance on {HileType,msg}
	//     NDIGFGBCINJ.ALNAIEKLDCN          0x7D9E930   JCFICNGFDLA* (HileType,string,Nullable<int32>,bool)
	//     HEMJAIINNMO.COINLACDIKJ          0x8A648D0   void  (HileType, string) instance
	//
	//   Toxic (hooking crashed the client)
	//     ANMMDHBAEJA.Invoke               0xDA9A40    void  (HileType, string) — this RVA
	//       is ALSO used by APLGMJIHKDE.Invoke(EGDIDOCECLM, int64_t) — il2cpp shares
	//       delegate-Invoke stubs across compatible signatures, so hooking here catches
	//       every delegate routed through the same trampoline. Nop-returning breaks all
	//       of them. DO NOT re-enable without a per-caller check.
	//     Buffer.VerifySignature           0x79F8870   bool  (Ed448PubKey, ctx, sig) — this
	//       is a boot / autosave signature check; forcing true earlier caused a crash
	//       during startup. DO NOT re-enable.
}
