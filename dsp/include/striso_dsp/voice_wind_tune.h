// Tuning correction tables for voice_wind.h (samples of extra loop delay per preset, at notes
// 20 + 7k, k = 0..13), measured on the host with tools/harness/out/wind_tune.h.
static const real CORR[NUM_PRESETS][14] = {
	{0.90f, 0.92f, 0.91f, 0.90f, 0.88f, 0.82f, 0.70f, 0.48f, 0.38f, 0.26f, 0.17f, 0.16f, 0.20f, 0.18f},  // clarinet
	{1.62f, 1.61f, 1.58f, 1.53f, 1.41f, 1.16f, 0.75f, 0.67f, 0.45f, 0.30f, 0.20f, 0.18f, 0.23f, 0.21f},  // bass clarinet
	{-10.16f, -6.63f, -4.23f, -2.61f, -1.50f, -0.69f, -0.08f, 0.34f, 0.88f, 0.69f, 0.47f, 0.42f, 0.55f, -0.36f},  // soprano sax
	{-10.13f, -6.60f, -4.19f, -2.55f, -1.42f, -0.61f, -0.19f, 0.59f, 1.02f, 0.69f, 0.47f, 0.42f, 0.55f, -0.20f},  // alto sax
	{-10.54f, -6.82f, -4.28f, -2.54f, -1.54f, -0.84f, 0.20f, 1.10f, 1.10f, 0.73f, 0.49f, 0.44f, 0.58f, -0.30f},  // tenor sax
	{-10.90f, -6.97f, -4.28f, -2.93f, -1.76f, -0.32f, 0.89f, 1.77f, 1.15f, 0.77f, 0.52f, 0.47f, 0.61f, -0.32f},  // baritone sax
	{-6.95f, -4.63f, -2.98f, -1.86f, -1.10f, -0.56f, -0.21f, 0.08f, 0.50f, 0.59f, 0.39f, 0.34f, 0.41f, 0.42f},  // oboe
	{-4.13f, -2.80f, -1.60f, -1.48f, -0.29f, 0.52f, 0.76f, 0.80f, 0.63f, 0.42f, 0.28f, 0.24f, 0.29f, 0.30f},  // bassoon
	{4.04f, 4.04f, 4.04f, 4.04f, 4.04f, 4.04f, 0.06f, 0.07f, 0.02f, -0.01f, -0.01f, -0.00f, -0.00f, 0.00f},  // flute
};
