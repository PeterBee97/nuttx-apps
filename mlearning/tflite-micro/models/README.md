# Ethos-U VWW96 model

`vww96_u55_256.tflite` is a Visual Wake Words person/not-person model
compiled for Ethos-U55-256.

## Provenance

- Upstream repository: `https://github.com/renesas/ruhmi-model-zoo`
- Upstream revision: `fe7e84ec6823e9abdfd0553361cf20b1c5dbee42`
- Source file: `vision/image_classification/visualwakeword/python/model/vww_96_INT8.tflite`
- Source SHA-256:
  `e33215c8d892b1412cc317ab4deff8afcc4f5dd1f56db35b2f4ab2031cf6c056`
- Compiled SHA-256:
  `2747d0718ff37518e9e742f4b5bb757baa6caa0007e499b3cdee8e8355a1e9d2`

The model metadata records Vela 4.0.1, accelerator `ethos-u55-256`,
memory mode `Sram_Only`, system configuration
`Ethos_U55_High_End_Embedded`, and performance optimization. The original
Vela configuration file is unavailable, so these settings do not guarantee
a byte-identical rebuild.

The input is INT8 RGB `[1, 96, 96, 3]` with scale `1/255` and zero point
`-128`. The two INT8 outputs are `notperson` and `person`.

The model license is Apache-2.0. See `LICENSE.vww96` and `NOTICE.vww96`.
