deb:
	apt-get update
	apt-get install -y g++ make libfuse3-dev fuse3 pkg-config dpkg-dev file
	g++ -O2 -std=c++17 -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=35 \
		-o kubsh main.cpp vfs.cpp -static -lfuse3 -pthread -ldl
	mkdir -p deb_build/usr/local/bin
	cp kubsh deb_build/usr/local/bin/
	mkdir -p deb_build/DEBIAN
	echo "Package: kubsh" > deb_build/DEBIAN/control
	echo "Version: 1.0" >> deb_build/DEBIAN/control
	echo "Architecture: amd64" >> deb_build/DEBIAN/control
	echo "Maintainer: $$USER" >> deb_build/DEBIAN/control
	echo "Description: Simple shell with VFS using FUSE3 (statically linked)" >> deb_build/DEBIAN/control
	echo "Depends: " >> deb_build/DEBIAN/control
	echo "" >> deb_build/DEBIAN/control
	dpkg-deb --build deb_build kubsh.deb
	echo "Package created: kubsh.deb"
