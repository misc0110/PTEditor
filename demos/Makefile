all: memmap nx map_pt uncachable virt2phys

memmap: memmap.c
	gcc memmap.c -o memmap

nx: nx.c
	gcc nx.c -o nx

map_pt: map_pt.c
	gcc map_pt.c -o map_pt
	
uncachable: uncachable.c
	gcc uncachable.c -o uncachable
	
virt2phys: virt2phys.c
	gcc virt2phys.c -o virt2phys

clean:
	rm -f memmap nx map_pt uncachable virt2phys
