#! /bin/sh -e

# Not sure how I didn't find this one before - so we have two files that
# include a header. We touch the header, and the first file fails to compile.
# Then we fix the compilation error by changing the C file (not the header) and
# update - the second file won't ever be rebuilt. Obviously, that is incorrect.
. ../tup.sh
tup config num_jobs 1
cat > Tupfile << HERE
: foreach *.c |> gcc -c %f -o %o |> %F.o
HERE

echo '#include "foo.h"' > foo.c
echo '#include "foo.h"' > bar.c
touch foo.h
tup touch foo.c bar.c foo.h
update
check_exist foo.o bar.o
rm -f bar.o
echo 'bork' >> foo.c
tup touch foo.h
update_fail
check_not_exist bar.o

echo '#include "foo.h"' > foo.c
tup touch foo.c
update
check_exist foo.o bar.o