import 'dart:typed_data';

/// Mono audio decoded from a WAV file.
class WavData {
  const WavData({required this.sampleRate, required this.samples});

  final int sampleRate;
  final Float32List samples;
}

class WavFormatException implements Exception {
  const WavFormatException(this.message);

  final String message;

  @override
  String toString() => 'WavFormatException: $message';
}

const _formatPcm = 1;
const _formatFloat = 3;
const _formatExtensible = 0xFFFE;

/// Encodes mono samples as a 32-bit float WAV (lossless for the engine).
Uint8List encodeWavFloat32(Float32List samples, int sampleRate) {
  const headerSize = 12 + 26 + 12 + 8; // RIFF + fmt (18) + fact + data header
  final dataSize = samples.length * 4;
  final bytes = ByteData(headerSize + dataSize);
  var o = 0;
  void tag(String s) {
    for (final c in s.codeUnits) {
      bytes.setUint8(o++, c);
    }
  }

  void u32(int v) {
    bytes.setUint32(o, v, Endian.little);
    o += 4;
  }

  void u16(int v) {
    bytes.setUint16(o, v, Endian.little);
    o += 2;
  }

  tag('RIFF');
  u32(headerSize - 8 + dataSize);
  tag('WAVE');
  tag('fmt ');
  u32(18);
  u16(_formatFloat);
  u16(1); // channels
  u32(sampleRate);
  u32(sampleRate * 4); // byte rate
  u16(4); // block align
  u16(32); // bits per sample
  u16(0); // cbSize
  tag('fact');
  u32(4);
  u32(samples.length);
  tag('data');
  u32(dataSize);
  final out = bytes.buffer.asUint8List();
  out.setAll(o, samples.buffer.asUint8List(samples.offsetInBytes, dataSize));
  return out;
}

/// Decodes PCM 16/24/32-bit or 32-bit float WAV. Multi-channel files are
/// mixed down to mono.
WavData decodeWav(Uint8List data) {
  final bytes = ByteData.sublistView(data);
  String tagAt(int o) => String.fromCharCodes(data.sublist(o, o + 4));
  if (data.length < 12 || tagAt(0) != 'RIFF' || tagAt(8) != 'WAVE') {
    throw const WavFormatException('not a RIFF/WAVE file');
  }

  int? format, channels, sampleRate, bits;
  int? dataOffset, dataSize;
  var o = 12;
  while (o + 8 <= data.length) {
    final id = tagAt(o);
    final size = bytes.getUint32(o + 4, Endian.little);
    final body = o + 8;
    if (id == 'fmt ') {
      format = bytes.getUint16(body, Endian.little);
      channels = bytes.getUint16(body + 2, Endian.little);
      sampleRate = bytes.getUint32(body + 4, Endian.little);
      bits = bytes.getUint16(body + 14, Endian.little);
      if (format == _formatExtensible && size >= 26) {
        format = bytes.getUint16(body + 24, Endian.little); // sub-format GUID
      }
    } else if (id == 'data') {
      dataOffset = body;
      dataSize = size.clamp(0, data.length - body);
    }
    o = body + size + (size.isOdd ? 1 : 0);
  }

  if (format == null || channels == null || sampleRate == null || bits == null) {
    throw const WavFormatException('missing fmt chunk');
  }
  if (dataOffset == null || dataSize == null) {
    throw const WavFormatException('missing data chunk');
  }
  if (channels < 1) throw const WavFormatException('no channels');

  final bytesPerSample = bits ~/ 8;
  final supported =
      (format == _formatPcm && (bits == 16 || bits == 24 || bits == 32)) ||
      (format == _formatFloat && bits == 32);
  if (!supported) {
    throw WavFormatException('unsupported format $format / $bits-bit');
  }

  double read(int at) {
    if (format == _formatFloat) return bytes.getFloat32(at, Endian.little);
    switch (bits) {
      case 16:
        return bytes.getInt16(at, Endian.little) / 32768.0;
      case 24:
        var v = bytes.getUint8(at) | (bytes.getUint8(at + 1) << 8) | (bytes.getUint8(at + 2) << 16);
        if (v & 0x800000 != 0) v -= 0x1000000;
        return v / 8388608.0;
      default:
        return bytes.getInt32(at, Endian.little) / 2147483648.0;
    }
  }

  final frameSize = bytesPerSample * channels;
  final frames = dataSize ~/ frameSize;
  final samples = Float32List(frames);
  for (var f = 0; f < frames; f++) {
    final at = dataOffset + f * frameSize;
    var sum = 0.0;
    for (var c = 0; c < channels; c++) {
      sum += read(at + c * bytesPerSample);
    }
    samples[f] = sum / channels;
  }
  return WavData(sampleRate: sampleRate, samples: samples);
}
