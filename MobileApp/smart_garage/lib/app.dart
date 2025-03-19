import 'package:flutter/material.dart';
import 'package:smart_garage/src/screens/home_screen.dart';

class MyApp extends StatelessWidget {
  const MyApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'SmartGarage',
      theme: ThemeData.dark().copyWith(),
      home: const HomeScreen(),
      debugShowCheckedModeBanner: false,
    );
  }
}
