import subprocess
import enroll

from i18n import _
from gi.repository import Gtk as gtk
from gi.repository import Gdk as gdk
from gi.repository import GObject as gobject


def on_user_change(self, select):
    self.active_user = select.get_active_text()
    self.load_model_list()


def on_user_add(self, button):
    # Open question dialog
    dialog = gtk.MessageDialog(
        parent=self,
        flags=gtk.DialogFlags.MODAL,
        type=gtk.MessageType.QUESTION,
        buttons=gtk.ButtonsType.OK_CANCEL
    )
    dialog.set_title(_("Confirm User Creation"))
    dialog.set_markup(
        _("Please enter the username of the user you want to add to Howdy")
    )

    # Create the input field
    entry = gtk.Entry()

    # Add a label to ask for a username
    hbox = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=5)
    hbox.pack_start(
        gtk.Label(label=_("Username:")),
        False,
        False,
        5
    )
    hbox.pack_start(entry, True, True, 5)

    # Add the box to the dialog
    content_area = dialog.get_content_area()
    content_area.pack_start(hbox, True, True, 5)

    dialog.show_all()

    # Show dialog
    response = dialog.run()

    entered_user = entry.get_text()
    dialog.destroy()

    if response == gtk.ResponseType.OK:
        self.userlist.append_text(entered_user)
        self.userlist.set_active(self.userlist.items)

        # Keep compatibility with the existing userlist implementation.
        self.userlist.items += 1

        self.active_user = entered_user
        self.load_model_list()


def on_model_add(self, button):
    if self.userlist.items == 0:
        return

    # Open question dialog
    dialog = gtk.MessageDialog(
        parent=self,
        flags=gtk.DialogFlags.MODAL,
        type=gtk.MessageType.QUESTION,
        buttons=gtk.ButtonsType.OK_CANCEL
    )
    dialog.set_title(_("Confirm Model Creation"))
    dialog.set_markup(
        _("Please enter a name for the new model, 24 characters max")
    )

    # Create the input field
    entry = gtk.Entry()

    # Add a label to ask for a model name
    hbox = gtk.Box(orientation=gtk.Orientation.HORIZONTAL, spacing=5)
    hbox.pack_start(
        gtk.Label(label=_("Model name:")),
        False,
        False,
        5
    )
    hbox.pack_start(entry, True, True, 5)

    # Add the box to the dialog
    content_area = dialog.get_content_area()
    content_area.pack_start(hbox, True, True, 5)

    dialog.show_all()

    # Show dialog
    response = dialog.run()

    entered_name = entry.get_text()
    dialog.destroy()

    if response == gtk.ResponseType.OK:
        # GTK 3 replacement for gtk4compat.PromptWindow
        dialog = gtk.Dialog(
            title=_("Creating Model"),
            parent=self.window,
            flags=gtk.DialogFlags.MODAL
        )

        dialog.set_resizable(False)
        dialog.set_default_size(420, -1)

        # 1. Get the primary monitor safely
        display = gdk.Display.get_default()
        monitor = display.get_primary_monitor()
        
        # Fallback if no primary monitor is set
        if not monitor:
            monitor = display.get_monitor(0)
            
        geometry = monitor.get_geometry()
        
        # 2. Get the default/requested size of the dialog
        # (Since it's not drawn yet, use get_default_size)
        width, height = dialog.get_default_size()
        
        # 3. Calculate positioning
        center_x = geometry.x + (geometry.width // 2) - (width // 2)
        top_y = 0

        dialog.move(center_x, top_y)

        content_area = dialog.get_content_area()

        # Main content
        box = gtk.Box(
            orientation=gtk.Orientation.VERTICAL,
            spacing=10
        )
        box.set_margin_top(20)
        box.set_margin_bottom(20)
        box.set_margin_start(20)
        box.set_margin_end(20)

        content_area.pack_start(box, True, True, 0)

        info_label = gtk.Label(
            label=_(
                "Keep your face inside the camera view and follow "
                "the prompts; this takes about a minute."
            )
        )
        info_label.set_line_wrap(True)
        info_label.set_max_width_chars(48)
        info_label.set_xalign(0.0)

        box.pack_start(info_label, False, False, 0)

        guide_label = gtk.Label(
            label=_("Look straight at the camera")
        )
        guide_label.set_xalign(0.0)

        # GTK 3 replacement for add_css_class("heading")
        guide_label.get_style_context().add_class("heading")

        box.pack_start(guide_label, False, False, 0)

        progress_label = gtk.Label(label="")
        progress_label.set_xalign(0.0)

        # GTK 3 replacement for add_css_class("dim-label")
        progress_label.get_style_context().add_class("dim-label")

        box.pack_start(progress_label, False, False, 0)

        dialog.show_all()

        # Wait a bit to allow the user to read the dialog
        gobject.timeout_add(
            600,
            lambda: execute_add(
                self,
                dialog,
                entered_name,
                guide_label,
                progress_label
            )
        )


def execute_add(
    box,
    dialog,
    entered_name,
    guide_label=None,
    progress_label=None
):
    """Run `howdy add` on a worker thread; the dialog shows
    the live guidance.
    """

    def on_guide(key):
        text = enroll.guide_text(key)

        if guide_label is not None and text:
            guide_label.set_markup(text)

        return False

    def on_progress(count, total):
        if progress_label is not None:
            progress_label.set_markup(
                enroll.progress_text(count, total)
            )

        return False

    def on_done(status, output):
        dialog.destroy()

        if status != 0:
            error_dialog = gtk.MessageDialog(
                parent=box.window,
                flags=gtk.DialogFlags.MODAL,
                type=gtk.MessageType.ERROR,
                buttons=gtk.ButtonsType.CLOSE
            )

            error_dialog.set_title(_("Howdy Error"))
            error_dialog.set_markup(
                _("Error while adding model, error code {}:").format(status)
            )
            error_dialog.format_secondary_text(output)

            error_dialog.run()
            error_dialog.destroy()

        box.load_model_list()
        return False

    enroll.run_add(
        [
            "howdy",
            "--gui",
            "-y",
            "-U",
            box.active_user,
            "add",
            entered_name
        ],
        on_guide,
        on_progress,
        on_done
    )

    return False


def on_model_delete(self, button):
    selection = self.treeview.get_selection()
    listmodel, rowlist = selection.get_selected_rows()

    if len(rowlist) == 1:
        tree_iter = listmodel.get_iter(rowlist[0])

        model_id = listmodel.get_value(tree_iter, 0)
        name = listmodel.get_value(tree_iter, 2)

        dialog = gtk.MessageDialog(
            parent=self,
            flags=gtk.DialogFlags.MODAL,
            buttons=gtk.ButtonsType.OK_CANCEL
        )
        dialog.set_title(_("Confirm Model Deletion"))
        dialog.set_markup(
            _("Are you sure you want to delete model {id} ({name})?")
            .format(id=model_id, name=name)
        )

        response = dialog.run()
        dialog.destroy()

        if response == gtk.ResponseType.OK:
            status, output = subprocess.getstatusoutput(
                "howdy remove {} -y -U {}".format(
                    model_id,
                    self.active_user
                )
            )

            if status != 0:
                error_dialog = gtk.MessageDialog(
                    parent=self,
                    flags=gtk.DialogFlags.MODAL,
                    type=gtk.MessageType.ERROR,
                    buttons=gtk.ButtonsType.CLOSE
                )

                error_dialog.set_title(_("Howdy Error"))
                error_dialog.set_markup(
                    _("Error while deleting model, error code {}:")
                    .format(status)
                )
                error_dialog.format_secondary_text(output)

                error_dialog.run()
                error_dialog.destroy()

            self.load_model_list()
