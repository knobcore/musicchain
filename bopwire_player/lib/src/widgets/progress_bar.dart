import 'package:flutter/material.dart';

/// Scrubbable playback progress bar. Builds a slider; while the user
/// is dragging the thumb the displayed position is the drag value, not
/// the player's reported `positionMs`, so it doesn't fight the touch.
/// On drag-end the caller's [onSeek] is invoked with the chosen ms.
class PlaybackProgressBar extends StatefulWidget {
  final int positionMs;
  final int durationMs;
  final Future<void> Function(int ms)? onSeek;

  const PlaybackProgressBar({
    super.key,
    required this.positionMs,
    required this.durationMs,
    this.onSeek,
  });

  @override
  State<PlaybackProgressBar> createState() => _PlaybackProgressBarState();
}

class _PlaybackProgressBarState extends State<PlaybackProgressBar> {
  double? _dragMs;

  String _format(int ms) {
    final total = ms ~/ 1000;
    final min   = total ~/ 60;
    final sec   = total % 60;
    return '$min:${sec.toString().padLeft(2, '0')}';
  }

  @override
  Widget build(BuildContext context) {
    final dur     = widget.durationMs > 0 ? widget.durationMs : 1;
    final current = (_dragMs ?? widget.positionMs.toDouble())
        .clamp(0.0, dur.toDouble());
    return Column(
      children: [
        SliderTheme(
          data: SliderTheme.of(context).copyWith(
            trackHeight: 4,
            overlayShape: SliderComponentShape.noOverlay,
          ),
          child: Slider(
            min:   0,
            max:   dur.toDouble(),
            value: current,
            onChanged: widget.onSeek == null
                ? null
                : (v) => setState(() => _dragMs = v),
            onChangeEnd: widget.onSeek == null
                ? null
                : (v) async {
                    final ms = v.round();
                    setState(() => _dragMs = null);
                    await widget.onSeek!(ms);
                  },
          ),
        ),
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceBetween,
          children: [
            Text(_format(current.round()),
                style: const TextStyle(fontSize: 12)),
            Text(_format(widget.durationMs),
                style: const TextStyle(fontSize: 12)),
          ],
        ),
      ],
    );
  }
}
