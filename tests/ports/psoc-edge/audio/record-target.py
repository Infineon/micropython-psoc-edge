from machine import PDM_PCM
import uio
import time

rx_buf = bytearray([0] * 128)


def record_audio(filename, time_sec):
    with uio.open(filename, "wb") as f:
        start_time = time.time()
        while time.time() - start_time < time_sec:  # record for "time_sec" second
            num_read = pdm_pcm.readinto(rx_buf)
            f.write(rx_buf)


pdm_pcm = PDM_PCM()

pdm_pcm.init()

# while not pdm_pcm.isready():
#     print(".", end="")

record_audio("audio.raw", 3)

print("mpy > audio recorded")
