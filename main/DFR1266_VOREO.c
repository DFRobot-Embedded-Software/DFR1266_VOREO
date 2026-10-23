#include <stdio.h>
#include "UserConfig.h"
#include "UserCode.h"
#include "PeripheralCode.h"
#include "RgbIndicator.h"

void app_main(void)
{
    initMIC();
    peripheralInit();
    //setRGBColor(RGB_BLACK);
    //autodetectSlotMapping();
#if LOCAL_DOA_ENABLE
    initAudioDoa();
#endif
#if LOCAL_ASR_WAKEUP_ENABLE
    initSpeechRecog();
#endif
}
