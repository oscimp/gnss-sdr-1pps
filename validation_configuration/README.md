## Test configurations

When activating the spoofing detection and cancellation capabitiliy, the filename is the basename
expected to complete with ``_1.bin`` and ``_2.bin`` for the signals collected by both antennas for CRPA.

``File-GNSS-SDR-receiver.conf`` decodes the data collected on a single record subject to spoofing.

```
$ gnss-sdr/build/src/main/gnss-sdr -c validation_configuration/File-GNSS-SDR-receiver.conf 
...
First position fix at 2019-Nov-23 00:01:00.140000 UTC is Lat = 48.3616 [deg], Long = -4.82146 [deg], Height= 271.285 [m]
Current receiver time: 55 s
Position at 2019-Nov-23 00:01:00.500000 UTC using 4 observations is Lat = 48.361859 [deg], Long = -4.822461 [deg], Height = 28.65 [m]
Velocity: East: -0.05 [m/s], North: 0.10 [m/s], Up = 0.18 [m/s]
The RINEX Navigation file header has been updated with UTC and IONO info.
Position at 2019-Nov-23 00:01:01.000000 UTC using 4 observations is Lat = 48.361918 [deg], Long = -4.822050 [deg], Height = 95.74 [m]
Velocity: East: 0.19 [m/s], North: 0.18 [m/s], Up = 0.42 [m/s]
Current receiver time: 56 s
```
is the spoofed location.

On the other hand,
``File-GNSS-SDR-receiver_spoof.conf`` analyzes both files collected during spoofing and aims at cancelling
the common signal to extract the genuine information.

```
$ gnss-sdr/build/src/main/gnss-sdr -c validation_configuration/File-GNSS-SDR-receiver.conf 
...
spoofing protection: 2
./4spoof191121_69dB_00h_m40dBm_1.bin ./4spoof191121_69dB_00h_m40dBm_2.bin  -- threshold = 0.050000
RF Channels: 1
Processing file ./4spoof191121_69dB_00h_m40dBm_1.bin, which contains 319872000 samples (2558976000 bytes) 
...
7:      stdargs=0.00013 1st estimate: 1.56987   0 1 2 3 4 5 6 *weightabs=0.97,weightarg=-1.58 /!\
First position fix at 2019-Nov-23 10:28:30.160000 UTC is Lat = 47.2518 [deg], Long = 5.99336 [deg], Height= 319.321 [m]
7:      stdargs=0.00001 1st estimate: 1.56092   0 1 2 3 4 5 6 *weightabs=0.98,weightarg=-1.58 /!\
Position at 2019-Nov-23 10:28:30.500000 UTC using 4 observations is Lat = 47.252715 [deg], Long = 5.994569 [deg], Height = 153.88 [m]
Velocity: East: 0.31 [m/s], North: -0.17 [m/s], Up = 0.96 [m/s]
Current receiver time: 2 min 57 s
Loss of lock in channel 0!
Position at 2019-Nov-23 10:28:31.000000 UTC using 4 observations is Lat = 47.250687 [deg], Long = 5.992268 [deg], Height = 568.28 [m]
Velocity: East: -1.81 [m/s], North: 1.38 [m/s], Up = 1.94 [m/s]
7:      stdargs=0.01370 1st estimate: 1.44691   0 1 2 3 4 5 *weightabs=0.99,weightarg=-1.68 /!\
Tracking of GPS L1 C/A signal started on channel 0 for satellite GPS PRN 01 (Block IIF)
```
is the genuine location.
