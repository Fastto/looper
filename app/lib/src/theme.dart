import 'package:flutter/material.dart';

/// Colours carry meaning in a looper: they must read from across the room.
abstract final class LooperColors {
  static const background = Color(0xFF0E0F13);
  static const surface = Color(0xFF181A21);
  static const surfaceHigh = Color(0xFF22252E);
  static const outline = Color(0xFF2E323D);
  static const textPrimary = Color(0xFFF2F3F5);
  static const textSecondary = Color(0xFF9097A6);

  static const record = Color(0xFFFF4D57);
  static const play = Color(0xFF3DDC97);
  static const overdub = Color(0xFFFF9F43);
  static const muted = Color(0xFF5A6070);
  static const downbeat = Color(0xFFFFB020);
  static const beat = Color(0xFFE8EAEE);
}

ThemeData buildLooperTheme() {
  final base = ThemeData(
    useMaterial3: true,
    brightness: Brightness.dark,
    colorScheme: ColorScheme.fromSeed(
      seedColor: LooperColors.play,
      brightness: Brightness.dark,
      surface: LooperColors.surface,
    ),
  );
  return base.copyWith(
    scaffoldBackgroundColor: LooperColors.background,
    textTheme: base.textTheme.apply(
      bodyColor: LooperColors.textPrimary,
      displayColor: LooperColors.textPrimary,
    ),
    sliderTheme: base.sliderTheme.copyWith(
      trackHeight: 3,
      thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 7),
      overlayShape: const RoundSliderOverlayShape(overlayRadius: 14),
    ),
    tooltipTheme: const TooltipThemeData(waitDuration: Duration(milliseconds: 600)),
  );
}
