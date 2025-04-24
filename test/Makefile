all: tests m1_tests

tests: tests.c utest.h ../ptedit_header.h
	gcc -Os tests.c -std=gnu99 -o tests -fsanitize=address

m1: tests.c utest.h ../ptedit_header.h
	gcc -Os m1_tests.c -std=gnu99 -o m1_tests -fsanitize=address

clean:
	rm -f tests m1_tests
	
