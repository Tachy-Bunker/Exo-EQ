#define PLUG_NAME "ExoEQ"
#define PLUG_MFR "Exomusica"
#define PLUG_VERSION_HEX 0x00010000
#define PLUG_VERSION_STR "1.0.0"
#define PLUG_UNIQUE_ID 'qBAd'
#define PLUG_MFR_ID 'Acme'
#define PLUG_URL_STR "https://iplug2.github.io"
#define PLUG_EMAIL_STR "spam@me.com"
#define PLUG_COPYRIGHT_STR "Copyright 2025 Acme Inc"
#define PLUG_CLASS_NAME ExoEQ

#define BUNDLE_NAME "ExoEQ"
#define BUNDLE_MFR "Exomusica"
#define BUNDLE_DOMAIN "com"

#define SHARED_RESOURCES_SUBPATH "ExoEQ"

#define PLUG_CHANNEL_IO "2.2-2"

#define PLUG_LATENCY 0
#define PLUG_TYPE 0
#define PLUG_DOES_MIDI_IN 0
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
#define PLUG_DOES_STATE_CHUNKS 0
#define PLUG_HAS_UI 1
// Circle-GUI round 12: 4:3 aspect ratio (the user's explicit call --
// "the ideal aspect ratio is 4:3 for this (horizontal largest)"), i.e.
// width:height = 4:3, width the larger dimension. Was 600x600 (1:1) --
// changed to 1000x750, which is exactly 4:3 (1000/750 = 4/3) and large
// enough to give the now-3-section layout (graph/detail panel/tab strip,
// see ExoEQ.cpp's OnLayout) comfortable room; 600px tall was already
// tight for 3 stacked sections plus a header row. OnLayout's own
// `pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false)` call
// scales width and height together on a manual drag-resize, so this
// starting ratio is also what gets preserved at any size the user drags
// to -- no separate aspect-lock code needed beyond setting it here.
#define PLUG_WIDTH 1000
#define PLUG_HEIGHT 750
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 0

#define AUV2_ENTRY ExoEQ_Entry
#define AUV2_ENTRY_STR "ExoEQ_Entry"
#define AUV2_FACTORY ExoEQ_Factory
#define AUV2_VIEW_CLASS ExoEQ_View
#define AUV2_VIEW_CLASS_STR "ExoEQ_View"

#define AAX_TYPE_IDS 'IEF1', 'IEF2'
#define AAX_TYPE_IDS_AUDIOSUITE 'IEA1', 'IEA2'
#define AAX_PLUG_MFR_STR "Acme"
#define AAX_PLUG_NAME_STR "ExoEQ\nIPEF"
#define AAX_PLUG_CATEGORY_STR "Effect"
#define AAX_DOES_AUDIOSUITE 1

#define VST3_SUBCATEGORY "Fx"

#define CLAP_MANUAL_URL "https://iplug2.github.io/manuals/example_manual.pdf"
#define CLAP_SUPPORT_URL "https://github.com/iPlug2/iPlug2/wiki"
#define CLAP_DESCRIPTION "A simple audio effect for modifying gain"
#define CLAP_FEATURES "audio-effect"//, "utility"

#define APP_NUM_CHANNELS 2
#define APP_N_VECTOR_WAIT 0
#define APP_MULT 1
#define APP_COPY_AUV3 0
#define APP_SIGNAL_VECTOR_SIZE 64

#define ROBOTO_FN "Roboto-Regular.ttf"
