all: tests

tests: tests.c utest.h ../ptedit_header.h
	gcc tests.c -o tests -fsanitize=address

clean:
	rm -f tests
	
