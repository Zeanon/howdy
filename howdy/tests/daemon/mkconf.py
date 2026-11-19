# mkconf.py <src config> <dst config> key=section.option=value ...
import sys, configparser

c = configparser.ConfigParser()
c.read(sys.argv[1])
for kv in sys.argv[3:]:
	k, v = kv.split("=", 1)
	s, o = k.split(".", 1)
	c.set(s, o, v)
with open(sys.argv[2], "w") as f:
	c.write(f)
