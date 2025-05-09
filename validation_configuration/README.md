## Test configurations

When activating the spoofing detection and cancellation capabitiliy, the filename is the basename
expected to complete with ``_1.bin`` and ``_2.bin`` for the signals collected by both antennas for CRPA.

``File-GNSS-SDR-receiver.conf`` decodes the data collected on a single record subject to spoofing.

``File-GNSS-SDR-receiver_spoof.conf`` analyzes both files collected during spoofing and aims at cancelling
the common signal to extract the genuine information.
