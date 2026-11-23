# Opens and controls main ui window
import gi
import os
import pwd
import sys
import signal
import asyncio
import builtins
import subprocess

from .i18n import S
from . import paths_factory

from howdy.cli import list  # type: ignore
from howdy.cli.auth import check_authorization  # type: ignore
from collections import namedtuple
from dbus_next import BusType  # type: ignore
from dbus_next.aio import MessageBus  # type: ignore

os.environ["GDK_BACKEND"] = "x11"

# Make sure we have the libs we need
gi.require_version("Gtk", "3.0")
from gi.repository import Gtk as gtk


Args = namedtuple("args", ["y", "gui", "plain", "user", "arguments"])


class MainWindow(gtk.Window):
	def __init__(self):
		"""Initialize the sticky window"""
		# Make the class a GTK window
		gtk.Window.__init__(self)

		self.builder = gtk.Builder()
		self.builder.add_from_file(paths_factory.main_window_wireframe_path())
		self.builder.connect_signals(self)

		self.window = self.builder.get_object("mainwindow")

		# The tabs draw their own top border, which would be a second line below
		# the separator on top of the window
		css = gtk.CssProvider()
		css.load_from_data(b"header.left tab { border-top-width: 0; }")
		self.builder.get_object("notebook").get_style_context().add_provider(
			css, gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
		)
		self.modellistbox = self.builder.get_object("modellistbox")

		# GTK3 has no glade property for rounded corners, so use CSS. The box paints
		# the (rounded) background, the treeview on top of it stays transparent
		radius_css = gtk.CssProvider()
		radius_css.load_from_data(
			b"""
			box.modellistbox {
				border: 1px solid @borders;
				border-radius: 8px;
				background-color: @theme_base_color;
			}
			box.modellistbox treeview.view {
				background-color: transparent;
				border-radius: 0;
			}
			box.modellistbox treeview.view.cell {
				background-color: transparent;
			}
			box.modellistbox treeview.view.cell:hover {
				background-color: alpha(@theme_selected_bg_color, 0.35);
			}
			box.modellistbox treeview.view.cell:selected {
				background-color: @theme_selected_bg_color;
			}
			box.modellistbox treeview.view.cell,
			box.modellistbox treeview.view.cell:selected,
			box.modellistbox treeview.view.cell:hover,
			box.modellistbox treeview header button {
				border-radius: 0;
			}
			box.modellistbox treeview header button:first-child {
				border-top-left-radius: 7px;
			}
			box.modellistbox treeview header button:last-child {
				border-top-right-radius: 7px;
			}
			"""
		)
		self.modellistbox.get_style_context().add_class("modellistbox")
		gtk.StyleContext.add_provider_for_screen(
			self.window.get_screen(),
			radius_css,
			gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
		)
		self.opencvimage = self.builder.get_object("opencvimage")

		self.window.connect("destroy", self.exit)
		self.window.connect("delete_event", self.exit)

		# Init capture for video tab
		self.capture = None

		# Create a treeview that will list the model data
		self.treeview = gtk.TreeView()
		self.treeview.set_vexpand(True)

		# Set the columns
		for i, column in enumerate([S("ID"), S("Created"), S("Label")]):
			renderer = gtk.CellRendererText()
			# The IDs are set in a little. As a padding of the cell, not as a space in the text: the text is
			# the ID that is sent to remove the model, and the daemon compares the IDs as text.
			if i == 0:
				renderer.set_padding(6, 0)
			col = gtk.TreeViewColumn(column, renderer, text=i)
			self.treeview.append_column(col)

		# Add the treeview
		self.modellistbox.add(self.treeview)

		# A right click on a model offers to rename or delete it. A popover that is not modal: a
		# menu grabs the pointer, and then the click on the close button of the window only
		# closes the menu.
		self.model_popover = gtk.Popover.new(self.treeview)
		self.model_popover.set_modal(False)
		box = gtk.Box(orientation=gtk.Orientation.VERTICAL, spacing=0)
		box.set_border_width(4)
		self.model_popover.add(box)
		for label, icon_name, handler in (
			(S("Rename"), "document-edit-symbolic", self.on_model_rename),
			(S("Delete"), "edit-delete-symbolic", self.on_model_delete),
		):
			# A flat button with the icon in front of the text, like the entries of a menu
			item = gtk.Button()
			item.set_relief(gtk.ReliefStyle.NONE)
			content = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=8)
			content.pack_start(
				gtk.Image.new_from_icon_name(icon_name, gtk.IconSize.MENU),
				False,
				False,
				0,
			)
			text = gtk.Label(label=label)
			text.set_xalign(0.0)
			content.pack_start(text, True, True, 0)
			item.add(content)
			# The list keeps the focus, losing it closes the menu
			item.set_focus_on_click(False)
			item.connect("clicked", self.on_model_menu_item, handler)
			box.pack_start(item, False, False, 0)
		box.show_all()
		self.treeview.connect("button-press-event", self.on_tree_button_press)
		self.treeview.connect("popup-menu", self.on_tree_popup_menu)
		self.treeview.connect("focus-out-event", self.on_tree_closes_menu)
		self.treeview.connect("key-press-event", self.on_tree_closes_menu)
		self.window.connect("focus-out-event", self.on_tree_closes_menu)
		self.builder.get_object("notebook").connect(
			"switch-page", lambda *_: self.model_popover.hide()
		)

		self.active_user = pwd.getpwuid(os.getuid()).pw_name

		self.window.show_all()

		self.load_model_list()

		# Start GTK main loop
		gtk.main()

	def load_model_list(self):
		"""(Re)load the model list"""

		# Create a datamodel
		self.listmodel = gtk.ListStore(str, str, str)

		builtins.howdy_args = Args(
			y=True, gui=True, plain=True, user=self.active_user, arguments=[]
		)

		user_models = list.run(gui=True)

		# Add the models to the datamodel
		for model in user_models:
			if len(model) < 3:
				continue
			self.listmodel.append((str(model[0]), str(model[1]), str(model[2])))

		self.treeview.set_model(self.listmodel)

	def on_about_link(self, label, uri):
		"""Open links on about page as a non-root user"""
		try:
			user = os.getlogin()
		except Exception:
			user = os.environ.get("SUDO_USER")

		subprocess.getstatusoutput(["sudo -u " + user + " timeout 10 xdg-open " + uri])
		return True

	def exit(self, widget=None, context=None):
		"""Cleanly exit"""
		self.release()

		gtk.main_quit()
		sys.exit(0)


try:
	asyncio.run(check_authorization())
except Exception as exc:
	print(f"Authorization failed: {exc}")
	sys.exit(1)

# Make sure we quit on a SIGINT
signal.signal(signal.SIGINT, signal.SIG_DFL)


async def has_models():
	"""Ask the management daemon whether the user has models. Only root can look into the
	models directory, so nobody else can tell by looking for it. Anything that goes wrong
	counts as yes: no reason to start the onboarding then."""
	builtins.howdy_args = Args(
		y=True,
		gui=True,
		plain=True,
		user=pwd.getpwuid(os.getuid()).pw_name,
		arguments=[],
	)
	bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
	try:
		return bool(await list.list_models(bus))
	except Exception:  # noqa: BLE001
		return True
	finally:
		bus.disconnect()


# If no models have been created yet or when it is forced, start the onboarding
if "--force-onboarding" in sys.argv or not asyncio.run(has_models()):
	from . import onboarding

	onboarding.OnboardingWindow()

	sys.exit(0)

# Class is split so it isn't too long, import split functions
from . import tab_models

MainWindow.on_model_add = tab_models.on_model_add
MainWindow.on_model_delete = tab_models.on_model_delete
MainWindow.on_model_rename = tab_models.on_model_rename
MainWindow.on_tree_button_press = tab_models.on_tree_button_press
MainWindow.on_tree_popup_menu = tab_models.on_tree_popup_menu
MainWindow.on_tree_closes_menu = tab_models.on_tree_closes_menu
MainWindow.on_model_menu_item = tab_models.on_model_menu_item
from . import tab_video

MainWindow.on_page_switch = tab_video.on_page_switch
MainWindow.capture_frame = tab_video.capture_frame
MainWindow.release = tab_video.release
MainWindow.__del__ = tab_video.__del__

# Open the GTK window
MainWindow()
