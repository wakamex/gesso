# A stand-in for a desktop's notification service, for testing gs_notify where none runs:
#   dbus-run-session -- sh -c 'uv run --with dbus-next tests/tools/notification_service.py LOG & sleep 2; GS_TEST_NOTIFY=LOG zig build test'
# It takes org.freedesktop.Notifications on the session bus with the methods and signals of the
# Desktop Notifications Specification, their argument types declared as the specification gives
# them, so a call with the wrong arguments is refused by the D-Bus library and never reaches it.
# Each call is written to LOG as a line of JSON. A notification whose body contains "[click]" is
# clicked 100 ms later (ActionInvoked with its "default" action), and a closed one is reported
# closed (NotificationClosed, reason 3). With "markup" after LOG it says it reads bodies as markup,
# as GNOME's and KDE's services do, and gs_notify must then escape them (GS_TEST_NOTIFY_MARKUP=1).
import asyncio, json, sys

from dbus_next import BusType, Variant
from dbus_next.aio import MessageBus
from dbus_next.service import ServiceInterface, method, signal


class Notifications(ServiceInterface):
    def __init__(self, log, markup):
        super().__init__("org.freedesktop.Notifications")
        self.log, self.last, self.markup = log, 0, markup

    def write(self, **call):
        self.log.write(json.dumps(call) + "\n")
        self.log.flush()

    @method()
    def Notify(self, app_name: "s", replaces_id: "u", app_icon: "s", summary: "s", body: "s", actions: "as", hints: "a{sv}", expire_timeout: "i") -> "u":
        if replaces_id:
            id = replaces_id
        else:
            self.last += 1
            id = self.last
        self.write(call="Notify", id=id, app_name=app_name, replaces_id=replaces_id, app_icon=app_icon, summary=summary, body=body, actions=actions, expire_timeout=expire_timeout)
        if "[click]" in body:
            asyncio.get_event_loop().call_later(0.1, lambda: self.ActionInvoked(id, "default"))
        return id

    @method()
    def CloseNotification(self, id: "u"):
        self.write(call="CloseNotification", id=id)
        self.NotificationClosed(id, 3)

    @method()
    def GetCapabilities(self) -> "as":
        return ["actions", "body"] + (["body-markup"] if self.markup else [])

    @method()
    def GetServerInformation(self) -> "ssss":
        return ["gesso test service", "gesso", "1", "1.2"]

    @signal()
    def ActionInvoked(self, id: "u", action_key: "s") -> "us":
        return [id, action_key]

    @signal()
    def NotificationClosed(self, id: "u", reason: "u") -> "uu":
        return [id, reason]


async def main():
    bus = await MessageBus(bus_type=BusType.SESSION).connect()
    bus.export("/org/freedesktop/Notifications", Notifications(open(sys.argv[1], "w"), "markup" in sys.argv[2:]))
    await bus.request_name("org.freedesktop.Notifications")
    await asyncio.get_event_loop().create_future()


asyncio.run(main())
