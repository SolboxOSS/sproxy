#rpmbuild -ba solhttpd.spec
%define _unpackaged_files_terminate_build 0
%define _missing_doc files_terminate_build 0
%define debug_package %{nil}
%define is_redhat %(test -e /etc/redhat-release && echo 1 || echo 0)
%define is_fedora %(test -e /etc/fedora-release && echo 1 || echo 0)
%define is_ubuntu %(test -e /etc/lsb-release && echo 1 || echo 0)
%define _topdir %(echo $HOME)/rpmbuild

%if %is_redhat
%define dist %(RET=$(cat /etc/redhat-release | grep CentOS |wc -l) ;if [ $RET == 1 ]; then  echo centos; else echo redhat;fi) 
%define disttag el
%endif
%if %is_fedora
%define dist fedora
%define disttag rhfc
%endif
%if %is_ubuntu
%define dist ubuntu
%define disttag sol
%endif

%define distver %(release="`rpm -q --queryformat='%{VERSION}' %{dist}-release 2> /dev/null | tr . : | sed s/://g`" ; if test $? != 0 ; then release="" ; fi ; echo "$release")

%define revision %(unset LC_ALL LANG; svn up|awk '{print $3}'|sed -e 's/\.$//g')

Name: solproxy 
Version: 0.9.0.1
Release: %{revision}.%{disttag}%{distver}
Summary: solproxy http caching server     

Group: Applications/Internet        
License: Private
Vendor: Solbox Inc.
URL: http://www.solbox.com           
Source: %{name}-%{version}.tar.gz
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-build
%if %is_ubuntu
Requires: db4.7-util libaio1 libdb4.7 curl geoip-bin
%else
Requires: GeoIP libaio db4-utils db4 curl 
%endif
AutoReqProv: no

%define NcPreFix /root/libnetcache/2.5.0

%description
create by Solbox, Inc.

%prep
%setup
./configure --enable-static=libconv --enable-shared --prefix=%{NcPreFix}

%build
make all 

%install
mkdir -p $RPM_BUILD_ROOT/usr/lib64

#make prefix=$RPM_BUILD_ROOT/usr/ install 
#cp -a %{NcLibPath}/libnc.so* $RPM_BUILD_ROOT/usr/lib64/
#cp -a %{NcLibPath}/libhttpn_driver.so* $RPM_BUILD_ROOT/usr/lib64/
#cp -a %{NcLibPath}/libjemalloc.so*  $RPM_BUILD_ROOT/usr/lib64/

make install
cp -a $RPM_BUILD_DIR/%{name}-%{version}/netcache/.libs//libnc.so*  $RPM_BUILD_ROOT/usr/lib64/
cp -a $RPM_BUILD_DIR/%{name}-%{version}/plugins/httpn/.libs/libhttpn_driver.so* $RPM_BUILD_ROOT/usr/lib64/
cp -a $RPM_BUILD_DIR/%{name}-%{version}/jemalloc-3.4.0/lib/libjemalloc.so*  $RPM_BUILD_ROOT/usr/lib64/

%post
%if !%is_ubuntu
if [ $1 == 1 ]; then # install
	/sbin/chkconfig --add solproxy
fi
%endif
%pre
if [ $1 == 2 ]; then # upgrade
      /sbin/service solproxy stop > /dev/null 2>&1
fi

%preun
if [ $1 == 0 ]; then # uninstall
      /sbin/service solproxy stop > /dev/null 2>&1
%if !%is_ubuntu
      /sbin/chkconfig --del solproxy
%endif
fi

%postun

%clean

%files
%defattr(-,root,root)
/usr/lib64/libhttpn_driver.so*
/usr/lib64/libnc.so*
/usr/lib64/libjemalloc.so*


%changelog

