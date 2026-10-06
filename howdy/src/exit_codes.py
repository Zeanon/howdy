from enum import Enum


class CompareResult(Enum):
	Success = 0,
	Error = 1,
	Abort = 10,
	Terminated = 11,
	NoFaceModel = 12,
	InvalidDevice = 13,
	TooDark = 14,
	TimeoutReached = 15,
	Rubberstamp = 20,
