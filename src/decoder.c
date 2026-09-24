#include "decoder.h"
#include "util.h"

#include <psp2/audiodec.h>
#include <psp2/sysmodule.h>

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

/* SceAudiodecInfo is a union, so its leading `size` field aliases the codec
 * specific one. psp2/audiodec.h documents `size` as the length of the
 * codec-specific struct (0x14 for SceAudiodecInfoAac), but the union is 0x1C,
 * so creation is retried with the other value before giving up. */
#define VM_ES_CAP  SCE_AUDIODEC_ROUND_UP(SCE_AUDIODEC_AAC_MAX_ES_SIZE)
#define VM_PCM_CAP SCE_AUDIODEC_ROUND_UP(SCE_AUDIODEC_AAC_MAX_SAMPLES * 2 * 2)

struct vm_dec {
    SceAudiodecCtrl ctrl;
    SceAudiodecInfo info;
    uint8_t *es;
    uint8_t *pcm;
    uint32_t es_cap;
    uint32_t pcm_cap;
    int channels;
    int sample_rate;
    int sbr;
    int created;
};

static int g_library_up;

int vm_dec_init(void)
{
    SceAudiodecInitParam param;
    int rc;

    if (g_library_up)
        return 0;

    sceSysmoduleLoadModule(SCE_SYSMODULE_AUDIOCODEC);

    memset(&param, 0, sizeof(param));
    param.aac.size = sizeof(SceAudiodecInitParam);
    param.aac.totalStreams = 1;

    rc = sceAudiodecInitLibrary(SCE_AUDIODEC_TYPE_AAC, &param);
    if (rc < 0) {
        vm_log("audiodec: init failed 0x%08X\n", (unsigned int)rc);
        return -1;
    }

    g_library_up = 1;
    return 0;
}

void vm_dec_shutdown(void)
{
    if (!g_library_up)
        return;
    sceAudiodecTermLibrary(SCE_AUDIODEC_TYPE_AAC);
    g_library_up = 0;
}

static void fill_info(vm_dec *dec, uint32_t size_value, int adts)
{
    memset(&dec->info, 0, sizeof(dec->info));
    dec->info.aac.size = size_value;
    dec->info.aac.isAdts = (SceUInt32)(adts ? 1 : 0);
    dec->info.aac.ch = (SceUInt32)dec->channels;
    dec->info.aac.samplingRate = (SceUInt32)dec->sample_rate;
    dec->info.aac.isSbr = (SceUInt32)(dec->sbr ? 1 : 0);
}

vm_dec *vm_dec_create(int channels, int sample_rate, int sbr, int adts)
{
    vm_dec *dec;
    int rc;

    if (vm_dec_init() != 0)
        return NULL;

    dec = calloc(1, sizeof(*dec));
    if (!dec)
        return NULL;

    dec->channels = channels > 0 ? channels : 2;
    dec->sample_rate = sample_rate > 0 ? sample_rate : 44100;
    dec->sbr = sbr;
    dec->es_cap = VM_ES_CAP;
    dec->pcm_cap = VM_PCM_CAP;

    dec->es = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, dec->es_cap);
    dec->pcm = memalign(SCE_AUDIODEC_ALIGNMENT_SIZE, dec->pcm_cap);
    if (!dec->es || !dec->pcm) {
        vm_log("audiodec: out of memory for codec buffers\n");
        vm_dec_destroy(dec);
        return NULL;
    }

    dec->ctrl.size = sizeof(SceAudiodecCtrl);
    dec->ctrl.wordLength = SCE_AUDIODEC_WORD_LENGTH_16BITS;
    dec->ctrl.pEs = dec->es;
    dec->ctrl.inputEsSize = 0;
    dec->ctrl.maxEsSize = dec->es_cap;
    dec->ctrl.pPcm = dec->pcm;
    dec->ctrl.outputPcmSize = 0;
    dec->ctrl.maxPcmSize = dec->pcm_cap;
    dec->ctrl.pInfo = (SceAudiodecInfo *)&dec->info;

    fill_info(dec, sizeof(SceAudiodecInfoAac), adts);
    rc = sceAudiodecCreateDecoder(&dec->ctrl, SCE_AUDIODEC_TYPE_AAC);
    if (rc < 0) {
        fill_info(dec, sizeof(SceAudiodecInfo), adts);
        rc = sceAudiodecCreateDecoder(&dec->ctrl, SCE_AUDIODEC_TYPE_AAC);
    }
    if (rc < 0) {
        vm_log("audiodec: create failed 0x%08X (%d Hz, %d ch)\n",
                 (unsigned int)rc, dec->sample_rate, dec->channels);
        vm_dec_destroy(dec);
        return NULL;
    }

    dec->created = 1;
    vm_log("audiodec: decoder ready (%d Hz, %d ch%s)\n", dec->sample_rate,
             dec->channels, dec->sbr ? ", SBR" : "");
    return dec;
}

void vm_dec_destroy(vm_dec *dec)
{
    if (!dec)
        return;
    if (dec->created)
        sceAudiodecDeleteDecoder(&dec->ctrl);
    free(dec->es);
    free(dec->pcm);
    free(dec);
}

void vm_dec_reset(vm_dec *dec)
{
    if (dec && dec->created)
        sceAudiodecClearContext(&dec->ctrl);
}

int vm_dec_decode(vm_dec *dec, const uint8_t *es, uint32_t es_size,
                    const int16_t **pcm, uint32_t *pcm_bytes)
{
    int rc;

    if (!dec || !dec->created || !es || es_size == 0)
        return -1;
    if (es_size > dec->es_cap)
        return -1;

    memcpy(dec->es, es, es_size);

    dec->ctrl.pEs = dec->es;
    dec->ctrl.inputEsSize = es_size;
    dec->ctrl.maxEsSize = dec->es_cap;
    dec->ctrl.pPcm = dec->pcm;
    dec->ctrl.outputPcmSize = 0;
    dec->ctrl.maxPcmSize = dec->pcm_cap;

    rc = sceAudiodecDecode(&dec->ctrl);
    if (rc < 0)
        return rc;

    if (pcm)
        *pcm = (const int16_t *)dec->pcm;
    if (pcm_bytes)
        *pcm_bytes = dec->ctrl.outputPcmSize;
    return 0;
}

int vm_dec_channels(const vm_dec *dec)
{
    if (dec->info.aac.ch > 0)
        return (int)dec->info.aac.ch;
    return dec->channels;
}

int vm_dec_sample_rate(const vm_dec *dec)
{
    if (dec->info.aac.samplingRate > 0)
        return (int)dec->info.aac.samplingRate;
    return dec->sample_rate;
}
