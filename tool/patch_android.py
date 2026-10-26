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
