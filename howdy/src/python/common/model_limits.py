# From this many models on, adding another one gets a warning: `howdy add` and the windows give
# it. The C++ version of this is src/cpp/common/model_limits.hh

from .i18n import S

MANY_MODELS = 3


def many_models(count):
	"""Whether a user with this many models gets the warning when adding another one"""
	return count > MANY_MODELS


def many_models_warning():
	"""What the warning says"""
	return S("Each additional model slows down the face recognition engine slightly")
