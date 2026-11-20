import syslog
import asyncio

from dbus_next.aio import MessageBus
from dbus_next import BusType

from .howdy_popup_interface import UIInterface
from .howdy_authentication_interface import (
	AuthenticationInterface,
	remember_sender,
)


BUS_NAME = "@authentication_bus@"
POPUP_PATH = "@popup_path@"
AUTHENTICATION_PATH = "@authentication_path@"


async def main():
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()

	# Init the services once.
	ui_service = UIInterface()
	auth_service = AuthenticationInterface(ui_service, bus)

	# The authentication methods need to know who calls
	bus.add_message_handler(remember_sender)

	bus.export(POPUP_PATH, ui_service)
	bus.export(AUTHENTICATION_PATH, auth_service)
	await bus.request_name(BUS_NAME)

	syslog.syslog(syslog.LOG_INFO, "Howdy Authentication D-Bus service is ready")

	# Keep the process and dbus-eventloop alive
	await asyncio.get_running_loop().create_future()


if __name__ == "__main__":
	asyncio.run(main())
