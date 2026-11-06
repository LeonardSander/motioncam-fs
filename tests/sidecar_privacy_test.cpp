#include "SidecarPrivacy.h"
#include <iostream>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int main() {
    try {
        const QByteArray pp3 = "[Exposure]\r\nCompensation=1.25\r\n[Exif]\r\nExif.GPSInfo.GPSLatitude=51/1\r\nExif.Image.GPSTag=123\r\nExif.Photo.LensModel=Keep lens\r\n[Exif.GPSInfo]\r\nLatitude=51\r\nLongitude=13\r\n[White Balance]\r\nTemperature=5000\r\n";
        const auto cleaned = motioncam::excludeSidecarGps(pp3, false);
        require(!cleaned.toLower().contains("gps") && !cleaned.contains("Latitude") &&
                    !cleaned.contains("Longitude"), "PP3 GPS keys and sections removed");
        require(cleaned.contains("Compensation=1.25\r\n") &&
                    cleaned.contains("LensModel=Keep lens\r\n") &&
                    cleaned.contains("Temperature=5000\r\n"), "PP3 editing settings preserved");
        const QByteArray plain = "[Exposure]\nCompensation=1.25\n\n";
        require(motioncam::excludeSidecarGps(plain, false) == plain, "GPS-free PP3 preserved");

        const QByteArray xml = "<x:edits xmlns:x='urn:edits' xmlns:exif='urn:exif' exposure='1.25' exif:GPSLatitude='51'><exif:GPSInfo><Latitude>51</Latitude><Longitude>13</Longitude></exif:GPSInfo><x:LensModel>Keep lens</x:LensModel><x:Curve points='1,2'/></x:edits>";
        const auto filtered = motioncam::excludeSidecarGps(xml, true);
        require(!filtered.contains("GPS") && !filtered.contains("Latitude") &&
                    !filtered.contains("Longitude"), "XML GPS attributes and subtrees removed");
        require(filtered.contains("Keep lens") && filtered.contains("exposure=\"1.25\"") &&
                    filtered.contains("points=\"1,2\""), "XML editing settings preserved");
        QXmlStreamReader parsed(filtered);
        while (!parsed.atEnd()) parsed.readNext();
        require(!parsed.hasError(), "Filtered XML remains valid");
        const QByteArray noGps = "<edits exposure='1.25'/>";
        require(motioncam::excludeSidecarGps(noGps, true) == noGps, "GPS-free XML preserved");
        bool rejected = false;
        try { motioncam::excludeSidecarGps("<edits><GPSLatitude>51", true); }
        catch (const std::runtime_error&) { rejected = true; }
        require(rejected, "Unreadable XML cannot bypass GPS filtering");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
