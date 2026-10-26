import 'package:flutter/material.dart';

import 'screens/scan_screen.dart';

void main() {
  runApp(const RadarloggerApp());
}

class RadarloggerApp extends StatelessWidget {
  const RadarloggerApp({super.key});

  @override
  Widget build(BuildContext context) {
    const seed = Color(0xFF2563EB);
    return MaterialApp(
      title: 'Radarlogger',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: seed),
        useMaterial3: true,
      ),
      darkTheme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: seed, brightness: Brightness.dark),
        useMaterial3: true,
      ),
      home: const ScanScreen(),
    );
  }
}
