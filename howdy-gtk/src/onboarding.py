import os
import re
import sys
import time
import fcntl
import shlex
import shutil
import struct
import threading
import subprocess

from .i18n import S
from . import paths_factory
from howdy import enroll_helper  # type: ignore

from gi.repository import Gtk as gtk
from gi.repository import GLib as glib
from gi.repository import Pango as pango


def privileged(argv):
	"""A command that needs root: pkexec asks for it unless this already is root"""
	if os.geteuid() == 0:
		return argv
	# pkexec only runs programs by their absolute path
	return [
		shutil.which("pkexec") or "pkexec",
		shutil.which(argv[0]) or argv[0],
	] + argv[1:]


def run_in_background(argv, on_line, on_done):
	"""Run a command without holding up the window, asking for root may take a while.
	on_line(line) gets its output, on_done(status, output) is called when it has
	ended, both in the UI thread. Returns a function that ends the command"""
	processes = []

	def work():
		lines = []
		try:
			with subprocess.Popen(
				argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT
			) as process:
				processes.append(process)
				for raw in process.stdout:
					lines.append(raw.decode("utf-8", "replace"))
					glib.idle_add(lambda line=lines[-1]: on_line(line) and False)
			status = process.returncode
		except OSError as exc:
			lines.append(str(exc))
			status = 127
		glib.idle_add(lambda: on_done(status, "".join(lines)) and False)

	threading.Thread(target=work, daemon=True).start()

	def stop():
		for process in processes:
			process.terminate()

	return stop


class OnboardingWindow(gtk.Window):
	def __init__(self):
		"""Initialize the sticky window"""
		# Make the class a GTK window
		gtk.Window.__init__(self)

		self.builder = gtk.Builder()
		self.builder.add_from_file(paths_factory.onboarding_wireframe_path())
		self.builder.connect_signals(self)
		# Ends `howdy add` while it runs
		self.stop_add = None

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

		step = {
			1: self.download_dlib_data,
			2: self.download_openvino_data,
			3: self.find_cameras,
			4: self.check_infrared_emitter,
			5: self.configure_camera,
			6: self.enable_next,
			7: self.finish,
		}.get(self.window.current_slide)
		if step is not None:
			step()

	def download_dlib_data(self):
		self.download_data_files(
			paths_factory.dlib_data_dir_path(),
			"shape_predictor_5_face_landmarks.dat",
			"downloadoutputlabel",
			"downloadeventbox",
			S("DLib datafiles have already been downloaded!\nClick Next to continue"),
		)

	def download_openvino_data(self):
		self.download_data_files(
			paths_factory.openvino_data_dir_path(),
			"face-detection-adas-0001.xml",
			"downloadoutputlabelopenvino",
			"downloadeventboxopenvino",
			S(
				"OpenVINO datafiles have already been downloaded!\nClick Next to continue"
			),
		)

	def download_data_files(self, directory, marker, label_id, box_id, done_text):
		label = self.builder.get_object(label_id)
		eventbox = self.builder.get_object(box_id)

		# Give the download output a black background
		css = gtk.CssProvider()
		css.load_from_data(b"* { background-color: black; }")
		eventbox.get_style_context().add_provider(
			css, gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
		)

		# TODO: Better way to do this?
		if os.path.exists(directory / marker):
			label.set_text(done_text)
			self.enable_next()
			return

		lines = []

		def show_line(line):
			print("install.sh output:")
			print(line)
			lines.append(line)
			del lines[:-10]
			label.set_text(" ".join(lines))

		def downloaded(status, output):
			if status != 0:
				self.show_error(S("Error while downloading datafiles"), " ".join(lines))
				return
			label.set_text(S("Done!\nClick Next to continue"))
			self.enable_next()

		# The data files are installed for the whole system
		run_in_background(
			privileged(
				["/bin/sh", "-c", f"cd {shlex.quote(str(directory))} && ./install.sh"]
			),
			show_line,
			downloaded,
		)

	def find_cameras(self):
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
			try:
				udevadm = subprocess.check_output(
					["udevadm", "info", "-r", "--query=all", "-n", device_path]
				).decode("utf-8", "replace")
			except (OSError, subprocess.CalledProcessError):
				# The camera keeps the name of its link
				udevadm = ""

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

	def selected_camera(self):
		"""(path, is infrared) of the camera that is selected, None without one"""
		(listmodel, rowlist) = self.treeview.get_selection().get_selected_rows()
		if len(rowlist) != 1:
			return None
		row = listmodel.get_iter(rowlist[0])
		return listmodel.get_value(row, 2), listmodel.get_value(row, 3)

	def check_infrared_emitter(self):
		try:
			import cv2
		except Exception:
			self.show_error(
				S("Error while importing OpenCV2"), S("Try reinstalling cv2")
			)

		selected = self.selected_camera()
		if selected is None:
			self.show_error(S("Error selecting camera"))
			return
		device_path, is_gray = selected

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

	def configure_camera(self):
		selected = self.selected_camera()
		if selected is None:
			self.show_error(S("Error selecting camera"))
			return

		# The scan can start when the camera is configured
		scanbutton = self.builder.get_object("scanbutton")
		scanbutton.set_sensitive(False)

		def configured(status, output):
			if status != 0:
				self.show_error(
					S("Error setting camera path"),
					S("Please set the camera path manually") + "\n\n" + output.strip(),
				)
				return
			scanbutton.set_sensitive(True)
			self.window.set_focus(scanbutton)

		run_in_background(
			privileged(["howdy", "set", "device_path", selected[0]]),
			print,
			configured,
		)

	def on_scanbutton_click(self, button):
		button.set_sensitive(False)
		self.dialog = gtk.MessageDialog(transient_for=self, modal=True)
		self.dialog.set_title(S("Creating Model"))
		self.dialog.props.text = S("Please look directly into the camera")
		# It goes away when the model is there, not by Escape
		self.dialog.connect("delete-event", lambda *_: True)
		self.dialog.show_all()

		# Wait a bit to allow the user to read the dialog
		glib.timeout_add(600, self.run_add)

	def run_add(self):
		def added(status, output):
			self.stop_add = None
			self.dialog.destroy()

			if status == enroll_helper.EXIT_CANCELLED:
				# The scan can be started again
				self.builder.get_object("scanbutton").set_sensitive(True)
				return
			if status != 0:
				self.show_error(S("Can't save face model"), output)
				return
			self.go_next_slide()

		self.stop_add = run_in_background(["howdy", "add", "-y"], print, added)

	def finish(self):
		radio_buttons = self.builder.get_object("radiobalanced").get_group()
		radio_selected = False
		# A percentage, the higher the stricter (as the C++ window has it)
		radio_certanty = 75

		for button in radio_buttons:
			if button.get_active():
				radio_selected = gtk.Buildable.get_name(button)

		if not radio_selected:
			self.show_error(S("Error reading radio buttons"))
		elif radio_selected == "radiofast":
			radio_certanty = 65
		elif radio_selected == "radiobalanced":
			radio_certanty = 75
		elif radio_selected == "radiosecure":
			radio_certanty = 88

		self.nextbutton.hide()
		self.builder.get_object("cancelbutton").hide()

		finishbutton = self.builder.get_object("finishbutton")
		finishbutton.show()
		# Not before it is saved: pkexec may still be asking for the password
		finishbutton.set_sensitive(False)

		def saved(status, output):
			if status != 0:
				self.show_error(
					S("Error setting certainty"),
					S("Certainty is set to the default value, Howdy setup is complete"),
				)
				return
			finishbutton.set_sensitive(True)
			self.window.set_focus(finishbutton)

		run_in_background(
			privileged(["howdy", "set", "certainty", str(radio_certanty)]),
			print,
			saved,
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
		if self.stop_add is not None:
			self.stop_add()
		gtk.main_quit()
		sys.exit(0)
