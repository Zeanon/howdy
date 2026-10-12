"""GTK 3 helpers.

Compatibility helpers for applications that used the GTK 4 helper API.
The public helper API is intentionally kept close to the GTK 4 version,
but all widgets and operations are implemented with GTK 3 APIs.
"""

import html
import re

from gi.repository import Gtk as gtk
from gi.repository import GLib
from gi.repository import GObject
from gi.repository import Pango


_main_loop = None


def run_main():
    """Block in the GTK 3 application main loop."""
    global _main_loop
    try:
        _main_loop = True
        gtk.main()
    finally:
        _main_loop = None


def quit_main():
    """Quit the current GTK 3 main loop, if one is running."""
    if _main_loop is not None and gtk.main_level() > 0:
        gtk.main_quit()


def run_dialog(dialog):
    """Show *dialog* and block until it responds.

    GTK 3 still provides Gtk.Dialog.run(), so no explicit GLib nested
    MainLoop is required.
    """
    if not dialog.get_visible():
        dialog.show_all()
    return dialog.run()


def alert(parent, heading, body="", buttons=("Close",), default=0, cancel=None):
    """Show a modal GTK 3 message dialog and return the selected button index."""
    if cancel is None:
        cancel = default

    dialog = gtk.MessageDialog(
        transient_for=parent,
        modal=True,
        message_type=gtk.MessageType.OTHER,
        buttons=gtk.ButtonsType.NONE,
    )
    dialog.set_title(heading)
    dialog.set_markup(GLib.markup_escape_text(heading))

    if body:
        dialog.format_secondary_text(body)

    for index, label in enumerate(buttons):
        dialog.add_button(label, index)

    dialog.set_default_response(default)

    # Escape/window close should behave like the requested cancel button.
    dialog.set_data("_gtk3_cancel_response", int(cancel))

    def on_delete(window, _event):
        window.response(int(cancel))
        return True

    handler = dialog.connect("delete-event", on_delete)

    try:
        response = run_dialog(dialog)
    finally:
        try:
            dialog.disconnect(handler)
        except Exception:
            pass
        dialog.destroy()

    if response in range(len(buttons)):
        return response
    return cancel


class PromptWindow(gtk.Dialog):
    """Small modal GTK 3 dialog with a content box and action buttons.

    The API intentionally mirrors the GTK 4 compatibility class:
    ``content``, ``actions``, ``add_action()`` and ``respond()`` are kept.
    """

    def __init__(self, parent, title):
        gtk.Dialog.__init__(
            self,
            title=title,
            transient_for=parent,
            modal=True,
        )

        self.set_resizable(False)
        self.set_default_size(420, -1)

        content_area = self.get_content_area()
        content_area.set_spacing(18)
        content_area.set_border_width(20)

        self.content = gtk.Box(
            orientation=gtk.Orientation.VERTICAL,
            spacing=10,
        )
        content_area.pack_start(self.content, True, True, 0)

        self.actions = self.get_action_area()
        self.actions.set_spacing(10)

        self.connect("delete-event", self._on_delete)

    def _on_delete(self, _dialog, _event):
        self.response(int(gtk.ResponseType.DELETE_EVENT))
        return True

    def add_action(self, label, response, suggested=False):
        button = self.add_button(label, int(response))
        if suggested:
            button.get_style_context().add_class("suggested-action")
            self.set_default_response(int(response))
        return button

    def respond(self, response):
        self.response(int(response))


class _Row(GObject.Object):
    """ListStore item carrying one row's cell values."""

    def __init__(self, values):
        GObject.Object.__init__(self)
        self.values = list(values)


class ColumnList:
    """Gtk.TreeView + Gtk.ListStore with a small list-like API.

    Replaces the GTK 4 ColumnView implementation with its GTK 3 equivalent.
    Rows remain plain value lists (``rows[i][col]``).
    """

    def __init__(self, titles, ncols=None):
        self.rows = []
        self.titles = list(titles)
        self.ncols = ncols or len(self.titles)

        # PyObject columns preserve arbitrary Python values.  The renderer
        # converts them to text when displaying them.
        self.store = gtk.ListStore(*([GObject.TYPE_PYOBJECT] * self.ncols))

        self.widget = gtk.TreeView(model=self.store)
        self.widget.set_vexpand(True)
        self.widget.set_hexpand(True)
        self.widget.get_style_context().add_class("data-table")
        self.widget.set_headers_visible(True)

        for index, title in enumerate(self.titles[:self.ncols]):
            renderer = gtk.CellRendererText()
            renderer.set_property("ellipsize", Pango.EllipsizeMode.END)

            column = gtk.TreeViewColumn(title, renderer)
            column.set_expand(True)
            column.set_resizable(True)
            column.add_attribute(renderer, "text", index)
            self.widget.append_column(column)

        self.selection = self.widget.get_selection()
        self.selection.set_mode(gtk.SelectionMode.SINGLE)

    def __len__(self):
        return len(self.rows)

    def __getitem__(self, index):
        return self.rows[index]

    def append(self, values):
        values = list(values)
        if len(values) < self.ncols:
            values += [None] * (self.ncols - len(values))
        self.rows.append(values)
        self.store.append(values[:self.ncols])

    def clear(self):
        self.rows = []
        self.store.clear()

    def select(self, index):
        if index is None or index < 0:
            self.selection.unselect_all()
            return

        path = gtk.TreePath.new_from_indices([int(index)])
        self.selection.select_path(path)
        self.widget.scroll_to_cell(
            path,
            None,
            True,
            0.5,
            0.0,
        )

    def selected_index(self):
        model, treeiter = self.selection.get_selected()
        if treeiter is None:
            return -1

        path = model.get_path(treeiter)
        indices = path.get_indices()
        return int(indices[0]) if indices else -1

    def selected_row(self):
        index = self.selected_index()
        return self.rows[index] if 0 <= index < len(self.rows) else None

    def connect_selection_changed(self, callback):
        self.selection.connect(
            "changed",
            lambda *_args: callback(self),
        )

    def scroll_to(self, index):
        if 0 <= index < len(self.rows):
            path = gtk.TreePath.new_from_indices([int(index)])
            self.widget.scroll_to_cell(path, None, True, 0.5, 0.0)


def iter_children(widget):
    """Yield the direct GTK 3 children of *widget*."""
    if isinstance(widget, gtk.Container):
        for child in widget.get_children():
            yield child


_TRANSLATABLE = re.compile(
    r'(<property\b[^>]*\btranslatable="(?:1|yes|true)"[^>]*>)(.*?)(</property>)',
    re.S,
)
_CONTEXT = re.compile(r'\bcontext="([^"]*)"')


def translate_ui_xml(xml, gettext_func, pgettext_func=None):
    """Translate translatable GtkBuilder properties in Python."""
    def repl(match):
        opening, text, closing = match.groups()
        if not text.strip():
            return match.group(0)

        source = html.unescape(text)
        context = _CONTEXT.search(opening)

        if context and pgettext_func is not None:
            translated = pgettext_func(context.group(1), source)
        else:
            translated = gettext_func(source)

        return (
            opening
            + html.escape(translated, quote=False)
            + closing
        )

    return _TRANSLATABLE.sub(repl, xml)


# GTK 4-only properties which are known to occur in the original UI files.
# GTK 3's GtkPicture has no content-fit property.
_CONTENT_FIT = re.compile(
    r'<property name="content-fit">[^<]*</property>',
    re.S,
)


def adapt_ui_xml(xml):
    """Remove GTK 4-only properties from UI XML before GTK 3 loads it."""
    return _CONTENT_FIT.sub("", xml)


def builder(scope, path, domain):
    """Create a GTK 3 GtkBuilder and connect signal handlers on *scope*.

    Unlike the GTK 4 BuilderScope approach, GTK 3/PyGObject provides the
    traditional ``connect_signals()`` API.
    """
    b = gtk.Builder()

    try:
        import i18n
        gettext_func = getattr(i18n, "_", None)
        translation = getattr(i18n, "translation", None)
        pgettext_func = getattr(translation, "pgettext", None)
    except Exception:
        gettext_func = None
        pgettext_func = None

    try:
        with open(path, encoding="utf-8") as fh:
            xml = fh.read()
    except OSError:
        b.set_translation_domain(domain)
        b.add_from_file(path)
        b.connect_signals(scope)
        return b

    if callable(gettext_func):
        xml = translate_ui_xml(xml, gettext_func, pgettext_func)
    else:
        b.set_translation_domain(domain)

    xml = adapt_ui_xml(xml)
    b.add_from_string(xml)
    b.connect_signals(scope)
    return b


def apply_text_direction(language_code):
    """Mirror the UI for right-to-left languages."""
    import languages

    rtl = languages.is_rtl(language_code)
    gtk.Widget.set_default_direction(
        gtk.TextDirection.RTL if rtl else gtk.TextDirection.LTR
    )
    return rtl


def gtk_expressions_supported():
    """GTK 3 has no Gtk.Expression/DropDown API."""
    return False


class IdDropDown:
    """Gtk.ComboBoxText-style adapter retaining the old DropDown API.

    GTK 3's ComboBoxText already supports stable string IDs, so no custom
    model/factory is necessary.
    """

    def __init__(self, widget, on_changed=None, min_width=0):
        self.widget = widget
        self._ids = []
        self.items = 0
        self._on_changed = on_changed

        if not isinstance(widget, gtk.ComboBoxText):
            raise TypeError(
                "IdDropDown expects a Gtk.ComboBoxText in GTK 3 UI files"
            )

        if min_width:
            widget.set_size_request(min_width, -1)

        widget.connect("changed", self._changed)

    def _changed(self, *_args):
        if self._on_changed is not None:
            self._on_changed(self)

    def remove_all(self):
        self.widget.remove_all()
        self._ids = []
        self.items = 0

    def append(self, id_, text):
        self.widget.append(str(id_), str(text))
        self._ids.append(id_)
        self.items += 1

    def append_text(self, text):
        self.append(text, text)

    def get_model(self):
        return self.widget.get_model()

    def set_active(self, index):
        self.widget.set_active(
            int(index) if index is not None and index >= 0 else -1
        )

    def get_active(self):
        return int(self.widget.get_active())

    def set_active_id(self, id_):
        return bool(self.widget.set_active_id(str(id_)))

    def get_active_id(self):
        value = self.widget.get_active_id()
        return value if value is not None else None

    def get_active_text(self):
        index = self.get_active()
        if index < 0:
            return None
        return self.widget.get_active_text()

    def __getattr__(self, name):
        return getattr(self.widget, name)


def close_popover(widget):
    """GTK 3 compatibility helper.

    GTK 3 ComboBox uses an internal popup rather than a Gtk.Popover child.
    If the widget exposes a popdown() method, use it.
    """
    popdown = getattr(widget, "popdown", None)
    if callable(popdown):
        try:
            popdown()
            return True
        except Exception:
            pass

    closed = False
    for child in iter_children(widget):
        if isinstance(child, gtk.Popover) and child.get_visible():
            child.popdown()
            closed = True
    return closed


def dropdown(widget, on_changed=None, min_width=0):
    """Wrap a Builder Gtk.ComboBoxText in an IdDropDown adapter."""
    adapter = getattr(widget, "_uh_dropdown", None)

    if adapter is None:
        adapter = IdDropDown(widget, on_changed, min_width)
        try:
            widget._uh_dropdown = adapter
        except Exception:
            pass
    elif on_changed is not None:
        adapter._on_changed = on_changed

    return adapter
