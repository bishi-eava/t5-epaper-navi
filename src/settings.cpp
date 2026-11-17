#include "settings.h"

#include <Preferences.h>

#include "board.h"

namespace settings {
namespace {

constexpr char kNamespace[] = "cfg";

uint8_t g_frontlight = 0;
uint16_t g_sleepMinutes = 3;
String g_lockImage;
String g_imageDir;
String g_transferSsid;
String g_transferPassword;
String g_routeKey;
MapView g_mapView;
int g_vcom = board::kVcomDefaultMv;

template <typename F>
void withPrefs(F fn)
{
    Preferences prefs;
    if (prefs.begin(kNamespace, false)) {
        fn(prefs);
        prefs.end();
    }
}

}  // namespace

void load()
{
    withPrefs([](Preferences &p) {
        g_frontlight = p.getUChar("light", 0);
        g_sleepMinutes = p.getUShort("sleep", 3);
        g_lockImage = p.isKey("lockimg") ? p.getString("lockimg", "") : String();
        g_imageDir = p.isKey("imgdir") ? p.getString("imgdir", "") : String();
        g_transferSsid = p.isKey("apssid") ? p.getString("apssid", "") : String();
        g_transferPassword = p.isKey("appass") ? p.getString("appass", "") : String();
        g_routeKey = p.isKey("routekey") ? p.getString("routekey", "") : String();
        g_vcom = p.getUShort("vcom", board::kVcomDefaultMv);
        if (p.isKey("maplat") && p.isKey("maplon")) {
            g_mapView.lat = p.getDouble("maplat", g_mapView.lat);
            g_mapView.lon = p.getDouble("maplon", g_mapView.lon);
            g_mapView.zoom = p.getUChar("mapzoom", g_mapView.zoom);
        }
    });
}

uint8_t frontlight() { return g_frontlight; }

void setFrontlight(uint8_t percent)
{
    g_frontlight = percent;
    withPrefs([&](Preferences &p) { p.putUChar("light", percent); });
}

uint16_t sleepMinutes() { return g_sleepMinutes; }

void setSleepMinutes(uint16_t minutes)
{
    g_sleepMinutes = minutes;
    withPrefs([&](Preferences &p) { p.putUShort("sleep", minutes); });
}

String lockImage() { return g_lockImage; }

void setLockImage(const String &path)
{
    g_lockImage = path;
    withPrefs([&](Preferences &p) { p.putString("lockimg", path); });
}

String imageDir() { return g_imageDir; }

void setImageDir(const String &dir)
{
    if (dir == g_imageDir) {
        return;  // 変わっていなければフラッシュに書かない
    }
    g_imageDir = dir;
    withPrefs([&](Preferences &p) { p.putString("imgdir", dir); });
}

String transferSsid() { return g_transferSsid; }

bool setTransferSsid(const String &ssid)
{
    if (ssid.length() < 1 || ssid.length() > 32) {
        return false;
    }
    g_transferSsid = ssid;
    withPrefs([&](Preferences &p) { p.putString("apssid", ssid); });
    return true;
}

String transferPassword() { return g_transferPassword; }

bool setTransferPassword(const String &password)
{
    if (password.length() < 8 || password.length() > 63) {
        return false;
    }
    g_transferPassword = password;
    withPrefs([&](Preferences &p) { p.putString("appass", password); });
    return true;
}

void resetTransfer()
{
    g_transferSsid = String();
    g_transferPassword = String();
    withPrefs([](Preferences &p) {
        p.remove("apssid");
        p.remove("appass");
    });
}

String routeKey() { return g_routeKey; }

bool setRouteKey(const String &key)
{
    String value = key;
    value.trim();
    if (value.length() > 128) {
        return false;
    }
    g_routeKey = value;
    withPrefs([&](Preferences &p) {
        if (value.length()) {
            p.putString("routekey", value);
        } else {
            p.remove("routekey");
        }
    });
    return true;
}

int vcom() { return g_vcom; }

void setVcom(int millivolts)
{
    g_vcom = constrain(millivolts, board::kVcomMinMv, board::kVcomMaxMv);
    withPrefs([&](Preferences &p) { p.putUShort("vcom", static_cast<uint16_t>(g_vcom)); });
}

MapView mapView() { return g_mapView; }

void setMapView(const MapView &view)
{
    if (view.lat == g_mapView.lat && view.lon == g_mapView.lon && view.zoom == g_mapView.zoom) {
        return;  // 変わっていなければフラッシュに書かない
    }
    g_mapView = view;
    withPrefs([&](Preferences &p) {
        p.putDouble("maplat", view.lat);
        p.putDouble("maplon", view.lon);
        p.putUChar("mapzoom", view.zoom);
    });
}

}  // namespace settings
