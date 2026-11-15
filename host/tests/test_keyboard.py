from m5_link.cli import Keyboard


def keyboard():
    return Keyboard(lines=[])  # typed lines are fed with type() instead of stdin


def test_lines_typed_before_connecting_are_sent_on_attach_in_order():
    keys, got = keyboard(), []
    keys.type("r\n")
    keys.type("s\n")
    keys.attach(got.append)
    keys.type("t\n")
    assert got == [b"r", b"s", b"t"]


def test_lines_are_queued_again_after_detach_or_a_failed_send():
    keys, got = keyboard(), []

    def broken(data):
        raise OSError("gone")

    keys.attach(broken)
    keys.type("a\n")  # send fails, so it waits for the next connection
    keys.detach()
    keys.type("b\n")
    keys.attach(got.append)
    assert got == [b"a", b"b"]
