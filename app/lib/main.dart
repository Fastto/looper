import 'package:flutter/material.dart';

import 'src/home_page.dart';
import 'src/looper_controller.dart';
import 'src/theme.dart';

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  final controller = LooperController()..start();
  runApp(LooperApp(controller: controller));
}

class LooperApp extends StatelessWidget {
  const LooperApp({super.key, required this.controller});

  final LooperController controller;

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Looper',
      debugShowCheckedModeBanner: false,
      theme: buildLooperTheme(),
      home: HomePage(controller: controller),
    );
  }
}
