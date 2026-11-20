import os
import re
import sys
import time
import fcntl
import struct
import threading
import subprocess

from .i18n import S
from . import paths_factory

from gi.repository import Gtk as gtk
from gi.repository import GLib as glib
from gi.repository import Pango as pango


class OnboardingWindow(gtk.Window):
	def __init__(self):
		"""Initialize the sticky window"""
		# Make the class a GTK window
		gtk.Window.__init__(self)

		self.builder = gtk.Builder()
		self.builder.add_from_file(paths_factory.onboarding_wireframe_path())
		self.builder.connect_signals(self)

		self.window = self.builder.get_object("onboardingwindow")
		self.slidecontainer = self.builder.get_object("slidecontainer")
		self.nextbutton = self.builder.get_object("nextbutton")

		self.window.connect("destroy", self.exit)
		self.window.connect("delete_event", self.exit)

		self.slides = [
			self.builder.get_object("slide0"),
			self.builder.get_object("slide1"),
			self.builder.get_object("slide2"),
			self.builder.get_object("slide3"),
			self.builder.get_object("slide4"),
			self.builder.get_object("slide5"),
			self.builder.get_object("slide6"),
			self.builder.get_object("slide7"),
		]

		self.window.show_all()
		self.window.resize(500, 400)

		self.window.current_slide = 0

		# Start GTK main loop
		gtk.main()

	def go_next_slide(self, button=None):
		self.nextbutton.set_sensitive(False)

		self.slides[self.window.current_slide].hide()
		self.slides[self.window.current_slide + 1].show()
		self.window.current_slide += 1
		# the shown child may have zero/wrong dimensions
		self.slidecontainer.queue_resize()

		if self.window.current_slide == 1:
			self.execute_slide1()
		elif self.window.current_slide == 2:
			glib.timeout_add(10, self.execute_slide2)
		elif self.window.current_slide == 3:
			glib.timeout_add(10, self.execute_slide3)
		elif self.window.current_slide == 4:
			self.execute_slide4()
		elif self.window.current_slide == 5:
			self.execute_slide5()
		elif self.window.current_slide == 6:
			self.execute_slide6()
		elif self.window.current_slide == 7:
			self.execute_slide7()

	def execute_slide1(self):
		self.downloadoutputlabel = self.builder.get_object("downloadoutputlabel")
		eventbox = self.builder.get_object("downloadeventbox")

		# Give the download output a black background
		css = gtk.CssProvider()
		css.load_from_data(b"* { background-color: black; }")
		eventbox.get_style_context().add_provider(
			css, gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
		)

		# TODO: Better way to do this?
		if os.path.exists(
			paths_factory.dlib_data_dir_path() / "shape_predictor_5_face_landmarks.dat"
		):
			self.downloadoutputlabel.set_text(
				S(
					"DLib datafiles have already been downloaded!\nClick Next to continue"
				)
			)
			self.enable_next()
			return

		self.proc = subprocess.Popen(
			"./install.sh",
			stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT,
			shell=True,
			cwd=paths_factory.dlib_data_dir_path(),
		)

		self.download_lines = []
		self.read_download_line()

	def read_download_line(self):
		line = self.proc.stdout.readline()
		self.download_lines.append(line.decode("utf-8"))

		print("install.sh output:")
		print(line.decode("utf-8"))

		if len(self.download_lines) > 10:
			self.download_lines.pop(0)

		self.downloadoutputlabel.set_text(" ".join(self.download_lines))

		if line:
			glib.timeout_add(10, self.read_download_line)
			return

		# Wait for the process to finish and check the status code
		if self.proc.wait(5) != 0:
			self.show_error(
				S("Error while downloading datafiles"), " ".join(self.download_lines)
			)

		self.downloadoutputlabel.set_text(S("Done!\nClick Next to continue"))
		self.enable_next()

	def execute_slide2(self):
		# Scan in a separate thread, opening every camera takes too long to do in the UI thread
		threading.Thread(target=self.scan_devices, daemon=True).start()

	def scan_devices(self):
		"""Find all cameras and check if they are infrared, runs outside of the UI thread"""

		def is_gray(frame):
			for row in frame:
				for pixel in row:
					if not pixel[0] == pixel[1] == pixel[2]:
						return False
			return True

		def is_capture_device(path):
			"""Check if the device can capture video, cameras also have metadata nodes that can't"""
			try:
				fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
				try:
					# Ask the driver for its 104 byte v4l2_capability struct (VIDIOC_QUERYCAP)
					caps = fcntl.ioctl(fd, 0x80685600, bytes(104))
				finally:
					os.close(fd)
			except OSError:
				# Let the regular checks report devices we can't query
				return True

			capabilities, device_caps = struct.unpack_from("II", caps, 84)

			# Use the capabilities of this specific device node if the driver provides them (V4L2_CAP_DEVICE_CAPS)
			if capabilities & 0x80000000:
				capabilities = device_caps

			# Check for V4L2_CAP_VIDEO_CAPTURE
			return bool(capabilities & 0x1)

		# GTK can only be used from the UI thread, so errors are passed on with idle_add
		try:
			import cv2
		except Exception:
			glib.idle_add(
				self.show_error,
				S("Error while importing OpenCV2"),
				S("Try reinstalling cv2"),
			)
			return

		device_rows = []
		try:
			device_ids = os.listdir("/dev/v4l/by-path")
		except Exception:
			glib.idle_add(
				self.show_error,
				S("No webcams found on system"),
				S(
					"Please configure your camera yourself if you are sure a compatible camera is connected"
				),
			)
			return

		# Devices that have already been checked, by-path can contain multiple links to the same device
		seen_devices = []

		# Loop though all devices, sorted so the same link is picked every time
		for dev in sorted(device_ids):
			# The full path to the device is the default name
			device_path = "/dev/v4l/by-path/" + dev
			device_name = dev

			# Skip links to a device we've already checked
			real_path = os.path.realpath(device_path)
			if real_path in seen_devices:
				continue
			seen_devices.append(real_path)

			# Skip metadata nodes and other devices that can't capture video
			if not is_capture_device(device_path):
				continue

			time.sleep(0.5)

			# Get the udevadm details to try to get a better name
			udevadm = subprocess.check_output(
				["udevadm info -r --query=all -n " + device_path], shell=True
			).decode("utf-8")

			# Loop though udevadm to search for a better name
			for line in udevadm.split("\n"):
				# Match it and encase it in quotes
				re_name = re.search("product.*=(.*)$", line, re.IGNORECASE)
				if re_name:
					# The kernel cuts names off at 31 characters, which can leave a trailing colon
					device_name = re_name.group(1).strip().rstrip(":").rstrip()

			capture = cv2.VideoCapture(device_path)
			is_open, frame = capture.read()
			if not is_open:
				device_rows.append(
					[device_name, device_path, -9, S("No, camera can't be opened")]
				)
				continue

			try:
				if not is_gray(frame):
					raise Exception()
			except Exception:
				device_rows.append(
					[device_name, device_path, -5, S("No, not an infrared camera")]
				)
				capture.release()
				continue

			device_rows.append(
				[device_name, device_path, 5, S("Yes, compatible infrared camera")]
			)
			capture.release()

		device_rows = sorted(device_rows, key=lambda k: -k[2])

		# Show the results from the UI thread
		glib.idle_add(self.show_devices, device_rows)

	def show_devices(self, device_rows):
		"""Show the list of found cameras"""
		self.loadinglabel = self.builder.get_object("loadinglabel")
		self.devicelistbox = self.builder.get_object("devicelistbox")

		self.treeview = gtk.TreeView()
		self.treeview.set_vexpand(True)

		# Set the columns
		for i, column in enumerate([S("Camera identifier or path"), S("Recommended")]):
			cell = gtk.CellRendererText()
			cell.set_property("ellipsize", pango.EllipsizeMode.END)
			col = gtk.TreeViewColumn(column, cell, text=i)
			self.treeview.append_column(col)

		# Add the treeview
		self.devicelistbox.add(self.treeview)

		# Create a datamodel
		self.listmodel = gtk.ListStore(str, str, str, bool)

		for device in device_rows:
			is_gray = device[2] == 5
			self.listmodel.append([device[0], device[3], device[1], is_gray])

		self.treeview.set_model(self.listmodel)
		self.treeview.set_cursor(0)

		self.treeview.show()
		self.loadinglabel.hide()
		self.enable_next()

	def execute_slide4(self):
		try:
			import cv2
		except Exception:
			self.show_error(
				S("Error while importing OpenCV2"), S("Try reinstalling cv2")
			)

		selection = self.treeview.get_selection()
		(listmodel, rowlist) = selection.get_selected_rows()

		if len(rowlist) != 1:
			self.show_error(S("Error selecting camera"))

		device_path = listmodel.get_value(listmodel.get_iter(rowlist[0]), 2)
		is_gray = listmodel.get_value(listmodel.get_iter(rowlist[0]), 3)

		if is_gray:
			# test if linux-enable-ir-emitter help should be displayed,
			# the user must click on the yes/no button which calls the method slide3_button_yes|no
			self.capture = cv2.VideoCapture(device_path)
			if not self.capture.isOpened():
				self.show_error(
					S("The selected camera cannot be opened"),
					S("Try to select another one"),
				)
			self.capture.read()
		else:
			# skip, the selected camera is not infrared
			self.go_next_slide()

	def slide3_button_yes(self, button):
		self.capture.release()
		self.go_next_slide()

	def slide3_button_no(self, button):
		self.capture.release()
		self.builder.get_object("leiestatus").set_markup(
			S(
				'Please visit\n<a href="https://github.com/EmixamPP/linux-enable-ir-emitter">https://github.com/EmixamPP/linux-enable-ir-emitter</a>\nto enable your ir emitter'
			)
		)
		self.builder.get_object("leieyesbutton").hide()
		self.builder.get_object("leienobutton").hide()

	def on_scanbutton_click(self, button):
		# status = self.proc.wait(2)

		# if status != 0:
		# 	self.show_error(S("Error setting camera path"), S("Please set the camera path manually"))

		self.dialog = gtk.MessageDialog(transient_for=self, modal=True)
		self.dialog.set_title(S("Creating Model"))
		self.dialog.props.text = S("Please look directly into the camera")
		self.dialog.show_all()

		# Wait a bit to allow the user to read the dialog
		glib.timeout_add(600, self.run_add)

	def run_add(self):
		status, output = subprocess.getstatusoutput(["howdy add -y"])

		print("howdy add output:")
		print(output)

		self.dialog.destroy()

		if status != 0:
			self.show_error(S("Can't save face model"), output)

		glib.timeout_add(10, self.go_next_slide)

	def execute_slide5(self):
		self.enable_next()

	def execute_slide6(self):
		self.enable_next()

	def execute_slide7(self):
		radio_buttons = self.builder.get_object("radiobalanced").get_group()
		radio_selected = False
		radio_certanty = 5.0

		for button in radio_buttons:
			if button.get_active():
				radio_selected = gtk.Buildable.get_name(button)

		if not radio_selected:
			self.show_error(S("Error reading radio buttons"))
		elif radio_selected == "radiofast":
			radio_certanty = 4.2
		elif radio_selected == "radiobalanced":
			radio_certanty = 3.5
		elif radio_selected == "radiosecure":
			radio_certanty = 2.2

		self.proc = subprocess.Popen(
			"howdy set certainty " + str(radio_certanty),
			stdout=subprocess.PIPE,
			stderr=subprocess.STDOUT,
			shell=True,
		)

		self.nextbutton.hide()
		self.builder.get_object("cancelbutton").hide()

		finishbutton = self.builder.get_object("finishbutton")
		finishbutton.show()
		self.window.set_focus(finishbutton)

		status = self.proc.wait(2)

		if status != 0:
			self.show_error(
				S("Error setting certainty"),
				S("Certainty is set to the default value, Howdy setup is complete"),
			)

	def enable_next(self):
		self.nextbutton.set_sensitive(True)
		self.window.set_focus(self.nextbutton)

	def show_error(self, error, secon=""):
		dialog = gtk.MessageDialog(
			transient_for=self,
			modal=True,
			message_type=gtk.MessageType.ERROR,
			buttons=gtk.ButtonsType.CLOSE,
		)
		dialog.set_title(S("Howdy Error"))
		dialog.props.text = error
		dialog.format_secondary_text(secon)

		dialog.run()

		dialog.destroy()
		self.exit()

	def exit(self, widget=None, context=None):
		"""Cleanly exit"""
		gtk.main_quit()
		sys.exit(0)
