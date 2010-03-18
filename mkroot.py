#!/usr/bin/python

from sys import stdin, argv
import os, stat

class foostr(str):
	def __lt__(a, b):
		if len(a) < len(b):
			return True
		if len(a) > len(b):
			return False
		return str.__lt__(a, b)
	def __le__(a, b):
		if len(a) < len(b):
			return True
		if len(a) > len(b):
			return False
		return str.__le__(a, b)
	def __gt__(a, b):
		if len(a) > len(b):
			return True
		if len(a) < len(b):
			return False
		return str.__gt__(a, b)
	def __ge__(a, b):
		if len(a) > len(b):
			return True
		if len(a) < len(b):
			return False
		return str.__ge__(a, b)
	def __eq__(a, b):
		return str.__eq__(a, b)
	def __ne__(a, b):
		return str.__ne__(a, b)
	def __cmp__(a, b):
		if len(a) != len(b):
			return len(a) - len(b)
		return str.__cmp__(a, b)

class FileObject:
	def __lt__(a, b):
		return a.size < b.size
	def __le__(a, b):
		return a.size <= b.size
	def __gt__(a, b):
		return a.size > b.size
	def __ge__(a, b):
		return a.size >= b.size
	def __ne__(a, b):
		return a.size != b.size
	def __eq__(a, b):
		return a.size == b.size
	def __cmp__(a, b):
		return a.size - b.size

	def __init__(self, path):
		st = os.stat(path)
		key = (st.st_dev, st.st_ino)
		self.key = key
		self.path = path
		self.size = st.st_size
	def __str__(self):
		return self.path
	def to_file(self, f):
		self.fpos = f.tell()

		# TODO: write header

		infile = open(self.path)
		st = os.fstat(infile.fileno())
		if (st.st_dev, st.st_ino) != self.key:
			raise Exception("Rug pulling mayhem")
		buf = infile.read(8192)
		while buf:
			f.write(buf)
			buf = infile.read(8192)

		self.flen = f.tell() - self.fpos

class ObjectStore:
	PATH_404 = "./404.html"

	def __init__(self):
		self.__obj_db = {}
		return

	def add(self, path = None):
		if not path:
			path = self.PATH_404

		candidate = FileObject(path)
		key = candidate.key

		if self.__obj_db.has_key(key):
			ret = self.__obj_db[key]
			del candidate
		else:
			self.__obj_db[key] = candidate
			ret = candidate

		return ret

	def write(self, fn):
		obj = self.__obj_db.values()
		obj.sort()
		f = open(fn, 'w')
		print "Writing object DB: %s"%fn
		for o in obj:
			o.to_file(f)
		return

class NameDB:
	def __init__(self):
		self.__names = {}
		self.__objects = ObjectStore()
	def __setitem__(self, key, val):
		self.__names[foostr(key)] = self.__objects.add(val)
	def __iter__(self):
		n = self.__names.keys()
		n.sort()
		return n.__iter__()
	def write(self, obj_fn, ndb_fn):
		self.__objects.write(obj_fn)
		f = open(ndb_fn, 'w')
		print "Writing name DB: %s"%ndb_fn
		for n in self:
			o = self.__names[n]
			f.write("\t{ ")
			f.write("{ .v_ptr = \"%s\"\n"%n)
			f.write("\t\t.v_len = %u},\n"%len(n))
			f.write("\t\t.f_ofs = %u, .f_len = %u"%(o.fpos, o.flen))
			f.write(" },\n")

def webmap(webroot, path):
	ret = os.path.join(webroot, path)

	try:
		st = os.stat(ret)
	except OSError:
		print "%s - 404"%ret
		return None

	if stat.S_ISDIR(st.st_mode):
		idx = os.path.join(ret, "index.html")
		try:
			st = os.stat(idx)
			return idx
		except OSError:
			return None

	return ret

if __name__ == '__main__':
	if len(argv) > 1:
		webroot = argv[1]
	else:
		webroot = '.'
	print "Webroot is %s"%webroot

	ndb = NameDB()
	for line in stdin:
		uri = line.rstrip("\r\n")
		path = uri.lstrip("/")
		ndb[uri] = webmap(webroot, path)
	
	ndb.write("webroot.objdb", "webroot.h")
