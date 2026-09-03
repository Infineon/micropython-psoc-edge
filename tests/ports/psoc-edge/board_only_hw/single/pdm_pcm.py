# This test is a sunny-side API check for PDM_PCM.
# It validates expected call flows, not data read correctness.
# No hardware microphone is connected, and no input data read
# validation is performed.
# The functional validation of the PDM_PCM can be done manually
# via the tests/ports/psoc-edge/audio recording utility

from machine import PDM_PCM

print("1. blocking read implementation ")

sck_pin_name = "P9_2"
data_pin_name = "P9_3"
sampling_rates = [16000]
frame_bits = [PDM_PCM.BITS_16, PDM_PCM.BITS_32]
formats = [PDM_PCM.MONO, PDM_PCM.STEREO]

for rate in sampling_rates:
    for format in formats:
        for bits in frame_bits:
            pdm_pcm = PDM_PCM(
                sck=sck_pin_name,
                data=data_pin_name,
                sample_rate=rate,
                bits=bits,
                format=format,
            )

            rx_buf = bytearray([0] * 64)
            num_read = pdm_pcm.readinto(rx_buf)
            print(
                f"data received for format = {format}, bits = {bits}, rate = {rate} : {num_read} bytes read"
            )
            pdm_pcm.deinit()


###############################################################################
print("\n2. irq non-blocking read implementation ")

rx_done = False


def rx_complete_irq(obj):
    global rx_done
    rx_done = True


pdm_pcm = PDM_PCM(
    sck=sck_pin_name,
    data=data_pin_name,
    sample_rate=16000,
    bits=PDM_PCM.BITS_16,
    format=PDM_PCM.STEREO,
)

rx_buf = bytearray([0] * 64)
pdm_pcm.irq(rx_complete_irq)
num_read = pdm_pcm.readinto(rx_buf)

while not rx_done:
    pass

# if we get pass this rx_done flag has been
# modified by the interrupt

print("rx blocking done")

pdm_pcm.deinit()

#############################
print("\n3. Set/Get gain")

pdm_pcm = PDM_PCM(
    sck=sck_pin_name,
    data=data_pin_name,
    sample_rate=16000,
    bits=PDM_PCM.BITS_16,
    format=PDM_PCM.STEREO,
)

pdm_pcm.gain(-50)
print(f"Get gain equal set gain: {pdm_pcm.gain() == -50}")
pdm_pcm.deinit()
