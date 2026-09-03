import machine


def freq_get_validate():
    freq = machine.freq()
    if freq == 200000000:
        print("PASS")
    else:
        print(freq)
        print("FAIL")


print("***** Test 1: Default Frequency Validation *****")
freq_get_validate()
