"""Lägger till Bluetooth-behörigheter och appnamn i det genererade Android-projektet."""
import re
from pathlib import Path

manifest = Path("android/app/src/main/AndroidManifest.xml")
s = manifest.read_text()

perms = """
    <uses-permission android:name="android.permission.BLUETOOTH_SCAN" android:usesPermissionFlags="neverForLocation" />
    <uses-permission android:name="android.permission.BLUETOOTH_CONNECT" />
    <uses-permission android:name="android.permission.ACCESS_FINE_LOCATION" android:maxSdkVersion="30" />
    <uses-permission android:name="android.permission.BLUETOOTH" android:maxSdkVersion="30" />
    <uses-permission android:name="android.permission.BLUETOOTH_ADMIN" android:maxSdkVersion="30" />
    <uses-feature android:name="android.hardware.bluetooth_le" android:required="true" />
"""
if "BLUETOOTH_SCAN" not in s:
    s = s.replace("<application", perms + "\n    <application", 1)

s = re.sub(r'android:label="[^"]*"', 'android:label="Radarlogger"', s, count=1)
manifest.write_text(s)
print(s)

# ------------------------------------------------------------
#  Fast signeringsnyckel för release-bygget
# ------------------------------------------------------------
kts = Path("android/app/build.gradle.kts")
groovy = Path("android/app/build.gradle")

if kts.exists():
    g = kts.read_text()
    signing = '''    signingConfigs {
        create("radar") {
            storeFile = file("${rootProject.projectDir}/../tool/signing/test.keystore")
            storePassword = "android"
            keyAlias = "androiddebugkey"
            keyPassword = "android"
        }
    }

    buildTypes {'''
    assert "buildTypes {" in g, "hittar inte buildTypes i build.gradle.kts"
    g = g.replace("    buildTypes {", signing, 1)
    assert 'signingConfigs.getByName("debug")' in g, "hittar inte signingConfig i build.gradle.kts"
    g = g.replace('signingConfigs.getByName("debug")', 'signingConfigs.getByName("radar")')
    kts.write_text(g)
    print("Signering inlagd i build.gradle.kts")
elif groovy.exists():
    g = groovy.read_text()
    signing = '''    signingConfigs {
        radar {
            storeFile file("${rootProject.projectDir}/../tool/signing/test.keystore")
            storePassword "android"
            keyAlias "androiddebugkey"
            keyPassword "android"
        }
    }

    buildTypes {'''
    g = g.replace("    buildTypes {", signing, 1)
    g = g.replace("signingConfig = signingConfigs.debug", "signingConfig = signingConfigs.radar")
    g = g.replace("signingConfig signingConfigs.debug", "signingConfig signingConfigs.radar")
    groovy.write_text(g)
    print("Signering inlagd i build.gradle")
else:
    raise SystemExit("Hittar ingen build.gradle")
