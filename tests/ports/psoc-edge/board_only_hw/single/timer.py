from machine import Timer
import time

oneshot_triggered = False
periodic_triggered = 0


# Callback functions for the timers to set the respective flags when triggered.
def call_oneshot(timer):
    global oneshot_triggered
    oneshot_triggered = True


# Callback function for the periodic timer to set the respective flag when triggered.
def call_periodic(timer):
    global periodic_triggered
    periodic_triggered += 1


# Oneshot timer
def test_oneshot():
    # Oneshot timer
    global oneshot_triggered
    tim_oneshot = Timer(0, period=100, mode=Timer.ONE_SHOT, callback=call_oneshot)

    try:
        # Wait for 5 seconds
        for i in range(300):
            time.sleep_ms(100)
            if oneshot_triggered:
                print("Oneshot timer triggered")
                oneshot_triggered = False
    finally:
        tim_oneshot.deinit()  # Deinitialize the Oneshot timer


# Periodic timer
def test_periodic():
    # Periodic timer
    global periodic_triggered
    timer_period_ms = 200
    tim_periodic = Timer(1, period=timer_period_ms, mode=Timer.PERIODIC, callback=call_periodic)

    try:
        delay_ms = 800
        time.sleep_ms(delay_ms)

        # One less trigger than expected as the Timer
        # might be deinitialized before the last callback is executed.
        expected_triggers = delay_ms // timer_period_ms - 1
        print("Periodic timer triggered:", periodic_triggered >= expected_triggers)

    finally:
        tim_periodic.deinit()  # Deinitialize the periodic timer


# Test that multiple timers can operate simultaneously without interference, and that their callbacks are triggered as expected.
def test_multiple_timers():
    global oneshot_triggered
    global periodic_triggered
    periodic_triggered = 0
    # Multiple timers
    timer_period_ms = 200
    tim_oneshot = Timer(0, period=100, mode=Timer.ONE_SHOT, callback=call_oneshot)
    tim_periodic = Timer(1, period=timer_period_ms, mode=Timer.PERIODIC, callback=call_periodic)

    try:
        delay_ms = 800
        time.sleep_ms(delay_ms)

        if oneshot_triggered:
            print("Oneshot timer triggered")

        expected_triggers = delay_ms // timer_period_ms - 1
        print("Periodic timer triggered:", periodic_triggered >= expected_triggers)

    finally:
        tim_oneshot.deinit()  # Deinitialize the Oneshot timer
        tim_periodic.deinit()  # Deinitialize the periodic timer


# Helper function to execute a test function and print the expected ValueError message.
def expect_value_error(label, fn):
    try:
        fn()
    except ValueError as e:
        print(label, e)


def test_frequency_input():
    global oneshot_triggered
    tim_freq = Timer(2, freq=10, mode=Timer.ONE_SHOT, callback=call_oneshot)

    try:
        time.sleep_ms(300)
        if oneshot_triggered:
            print("Frequency-based timer triggered")
            oneshot_triggered = False

    finally:
        tim_freq.deinit()

    # Verify Timer ID 3 (counter 3) can be constructed and deinitialized.
    tim_id3 = Timer(3, period=100, mode=Timer.ONE_SHOT, callback=call_oneshot)
    tim_id3.deinit()


# Negative test cases to validate that invalid parameters raise ValueError as expected.
def test_negative_cases():
    print("*****Negative Timer Parameter Tests*****")

    # Constructor boundary checks for invalid timer IDs.
    expect_value_error(
        "invalid_id_-1:",
        lambda: Timer(-1, period=1000, mode=Timer.ONE_SHOT, callback=call_oneshot),
    )
    expect_value_error(
        "invalid_id_32:",
        lambda: Timer(32, period=1000, mode=Timer.ONE_SHOT, callback=call_oneshot),
    )

    # Group-1 timer IDs (e.g. 8) are valid, but period is constrained by 16-bit counter width.
    expect_value_error(
        "id_8_16bit_limit:",
        lambda: Timer(8, period=1000, mode=Timer.ONE_SHOT, callback=call_oneshot),
    )

    # Constructor check for duplicate object creation on the same Timer ID.
    tdup = Timer(2, period=10000, mode=Timer.ONE_SHOT, callback=call_oneshot)
    try:
        expect_value_error(
            "timer_vs_timer_conflict:",
            lambda: Timer(2, period=1000, mode=Timer.ONE_SHOT, callback=call_oneshot),
        )
    finally:
        tdup.deinit()

    # Exercise init() validation paths without consuming additional timer IDs.
    tim = Timer(0)
    try:
        expect_value_error(
            "invalid_mode:", lambda: tim.init(mode=99, period=1000, callback=call_oneshot)
        )
        expect_value_error(
            "invalid_callback:", lambda: tim.init(mode=Timer.ONE_SHOT, period=1000, callback=1)
        )
        expect_value_error(
            "freq_zero:", lambda: tim.init(mode=Timer.ONE_SHOT, freq=0, callback=call_oneshot)
        )
        expect_value_error(
            "period_zero:", lambda: tim.init(mode=Timer.ONE_SHOT, period=0, callback=call_oneshot)
        )
        expect_value_error(
            "missing_freq_period:", lambda: tim.init(mode=Timer.ONE_SHOT, callback=call_oneshot)
        )
        expect_value_error(
            "period_ticks_zero:",
            lambda: tim.init(mode=Timer.ONE_SHOT, freq=2000000, callback=call_oneshot),
        )
    finally:
        tim.deinit()


# Test that a timer can be recreated after deinitialization, which should succeed without error.
def test_recreate_after_deinit():
    tim = Timer(0, period=10000, mode=Timer.ONE_SHOT, callback=call_oneshot)
    tim.deinit()

    tim_new = Timer(0, period=10000, mode=Timer.ONE_SHOT, callback=call_oneshot)
    tim_new.deinit()
    print("Recreate Timer(0) after deinit: OK")


def print_result(name, ok):
    print("{}: {}".format(name, "OK" if ok else "FAIL"))


# Test hard parameter behavior.
def test_hard_parameter_checks():
    print("*****Timer hard Parameter Tests*****")

    # 1) hard defaults to False (soft callback path).
    soft_events = []

    def cb_soft(timer):
        # callback with list append works.
        soft_events.append(1)

    tim_soft = Timer(0, period=50, mode=Timer.PERIODIC, callback=cb_soft)
    try:
        time.sleep_ms(250)
    finally:
        tim_soft.deinit()
    print_result("default_soft", len(soft_events) > 0)

    # 2) hard=True should execute callback — callback runs in ISR context.
    # Use a list element (pre-allocated) as a flag to avoid any heap
    # allocation inside the callback while GC is locked.
    hard_seen = [False]

    def cb_hard(timer):
        hard_seen[0] = True

    tim_hard = Timer(1, period=50, mode=Timer.PERIODIC, callback=cb_hard, hard=True)
    try:
        time.sleep_ms(250)
    finally:
        tim_hard.deinit()
    print_result("hard_true tested", hard_seen[0])

    # 3) hard must be bool.
    try:
        Timer(2, period=100, mode=Timer.ONE_SHOT, callback=call_oneshot, hard=1)
        print_result("hard_bool_validation", False)
    except ValueError:
        print_result("hard_bool_validation", True)
    except Exception:
        print_result("hard_bool_validation", False)

    # 4) hard accepts both bool values.
    hard_false_ok = False
    hard_true_ok = False

    try:
        t_bool = Timer(2, period=200, mode=Timer.ONE_SHOT, callback=call_oneshot, hard=False)
        t_bool.deinit()
        hard_false_ok = True
    except Exception:
        hard_false_ok = False

    try:
        t_bool = Timer(2, period=200, mode=Timer.ONE_SHOT, callback=call_oneshot, hard=True)
        t_bool.deinit()
        hard_true_ok = True
    except Exception:
        hard_true_ok = False

    print_result("hard_bool_values", hard_false_ok and hard_true_ok)


########## Main Execution ############

if __name__ == "__main__":
    print("*****Oneshot Timer Execution*****")
    test_oneshot()
    print("*****Periodic Timer Execution*****")
    test_periodic()
    print("*****Multiple Timers Execution*****")
    test_multiple_timers()
    test_frequency_input()
    test_negative_cases()
    test_recreate_after_deinit()
    test_hard_parameter_checks()
