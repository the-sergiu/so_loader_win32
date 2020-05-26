#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "exec_parser.h"

/* Handler vechi */
static struct sigaction old_handler;
/* Handler nou */
static struct sigaction handler;
/* File-descriptor-ul fisierului */
static int fd;
/* Dimensiunea unei pagini */
static int pageSize;

static so_exec_t *exec;

/*
 * Structura care tine evidenta paginilor alocate.
 * unsigned int * - vector de pagini care stocheaza
 * paginile mapate de-a lungul executiei.
 * Ne permite astfel si verificarea
 * paginilor deja mapate.
 *
 * unsigned int - tine evidenta numarului de pagini mapate,
 * un echivalent al 'size' in vectori.
 * La fiecare mapare de pagina, se incrementeaza.
 */
struct pageEvidence {
	unsigned int *pagesVect;
	unsigned int pageCount;
};

/* Handler-ul nou pentru tratarea semnalului SIGSEGV */
static void segv_handler(int signum, siginfo_t *info, void *context)
{
	/* Adrese */
	void *startAddr, *endAddr, *mmapAddr;
	/* Iteratori */
	unsigned int counter, pageIter;
	/* Variabile de verificare */
	int rc, already_mapped = 0;
	/* Informatii ale paginilor */
	int pageIndex, page_offset;
	/* Dimensiuni */
	int mem_size, file_size;
	/* Structuri */
	struct pageEvidence *pgEv;

	/* Daca se face acces invalid la memorie sau se primeste
	 * un alt semnal in afara de SIGSEGV, chemam handler-ul
	 * vechi, conform cerintei din tema.
	 */
	if (signum != SIGSEGV || info->si_code == SEGV_ACCERR) {
		sigemptyset(&old_handler.sa_mask);
		old_handler.sa_sigaction(signum, info, context);
		return;
	}

	/* Iteram prin toate segmentele */
	for (counter = 0; counter < exec->segments_no; counter++) {

		/* Retinem adresa de start a segmentului */
		startAddr = (void *) exec->segments[counter].vaddr;

		/* Retinem dimesiunea spatiului de adrese */
		mem_size = exec->segments[counter].mem_size;

		/* Retinem adresa de sfarsit a segmentului */
		endAddr = (void *) (startAddr + mem_size);

		/* Retinem datele(custom) din segment */
		pgEv = exec->segments[counter].data;

		/* Alocam spatiu pentru vectorul de pagini al structurii
		 * in cazul in care acesta nu a fost alocat deja. Operatia
		 * are loc o singura data, si anume la prima iteratie.
		 */
		if (pgEv->pagesVect == NULL) {
			pgEv->pagesVect = malloc((mem_size / pageSize + 1)
							 * sizeof(int));
			if (pgEv->pagesVect == NULL) {
				perror("Eroare alocare pagina!\n");
				exit(EXIT_FAILURE);
			}
		}

		/* Verificam daca adresa pe care o cautam se afla in
		 * intervalul segmentului curent. Daca nu, se apeleaza
		 * handler-ul vechi, iar apoi se trece la urmatorul segment.
		 */
		if (info->si_addr <= endAddr && info->si_addr >= startAddr) {

			/* Obtinem indexul paginii care a dat page fault */
			pageIndex = (info->si_addr - startAddr) / pageSize;

			/* Retinem dimensiunea fisierului */
			file_size = exec->segments[counter].file_size;

			/* Retinem offset-ul paginii */
			page_offset = exec->segments[counter].offset
						+ pageIndex * pageSize;

			/* Verificam daca pagina a fost deja mapata*/
			for (pageIter = 0; pageIter < pgEv->pageCount; ) {

				/* Daca o pagina din vectorul de pagini coincide
				 * cu o pagina pe care incercam sa o mapam
				 */
				if (pgEv->pagesVect[pageIter] == pageIndex)
					already_mapped = 1;

				pageIter++;
			}
			/* Daca pagina e deja mapata, apelam handler-ul vechi */
			if (already_mapped == 1) {
				old_handler.sa_sigaction(signum, info, context);
				return;
			}

			/* Mapam pagina */
			mmapAddr = mmap(startAddr + pageIndex * pageSize,
					pageSize, PROT_WRITE, MAP_SHARED
					| MAP_ANONYMOUS | MAP_FIXED, -1, 0);
			/* Errata: testele trec si fara MAP_FIXED */
			if (mmapAddr == (void *) -1) {
				perror("Eroare la maparea paginii!\n");
				exit(EXIT_FAILURE);
			}

			/* Citim din fisier, in functie de dimensiunea lui,
			 * in concordanta cu adresele/indexul paginilor
			 */
			if (file_size > pageIndex * pageSize) {
				if (file_size < (pageIndex + 1) * pageSize) {
					lseek(fd, page_offset, SEEK_SET);
					rc = read(fd, mmapAddr,
					file_size - pageIndex * pageSize);
					if (rc == -1) {
						perror("Eroare1 read!\n");
						exit(EXIT_FAILURE);
					}
				} else {
					lseek(fd, page_offset, SEEK_SET);
					rc = read(fd, mmapAddr, pageSize);
					if (rc == -1) {
						perror("Eroare2 read!\n");
						exit(EXIT_FAILURE);
					}
				}
			}

			/* Setam permisiunile paginii */
			rc = mprotect(mmapAddr, pageSize,
				exec->segments[counter].perm);
			if (rc == -1) {
				perror("Eroare mprotect!\n");
				exit(EXIT_FAILURE);
			}

			/* Adaugam pagina in vectorul de pagini, adica ii
			 * tinem evidenta. Incrementam contorul de pagini
			 * ca sa stim la ce numar de pagini am ajuns
			 */
			pgEv->pagesVect[pgEv->pageCount] = pageIndex;
			pgEv->pageCount++;
			return;
		}
	}

	/* Daca adresa nu s-a gasit in niciun segment,
	 * folosim handler-ul vechi
	 */
	old_handler.sa_sigaction(signum, info, context);
}


/* Initializam handler-ul nou pentru tratarea semnalului SIGSEGV*/
int so_init_loader(void)
{
	int rc;

	/* Retinem dimensiunea unei pagini */
	pageSize = getpagesize();

	/* Setam handler-ul */
	handler.sa_sigaction = segv_handler;

	/* Initializam noul handler */
	rc = sigemptyset(&handler.sa_mask);
	if (rc == -1) {
		perror("Eroare sigemptyset!\n");
		exit(EXIT_FAILURE);
	}
	/* Adauga abilitatea de a trata semnalul SIGSEGV */
	rc = sigaddset(&handler.sa_mask, SIGSEGV);
	if (rc == -1) {
		perror("Eroare sigaddset\n");
		exit(EXIT_FAILURE);
	}
	/* sa_sigaction specifica handler-ul, in
	 * locul sa_handler
	 */
	handler.sa_flags = SA_SIGINFO;

	rc = sigaction(SIGSEGV, &handler, &old_handler);
	if (rc == -1) {
		perror("Eroare sigaction\n");
		exit(EXIT_FAILURE);
	}

	return 0;
}

int so_execute(char *path, char *argv[])
{
	int counter;

	/* Deschidem fisierul ca sa copiem informatii
	 * din memorie
	 */
	fd = open(path, O_RDONLY);
	if (fd == -1) {
		perror("Eroare deschidere fisier!\n");
		exit(EXIT_FAILURE);
	}

	exec = so_parse_exec(path);
	if (exec == NULL) {
		perror("Eroare so_parse_exec!\n");
		exit(EXIT_FAILURE);
	}

	/* Initializam structura de evidenta a paginilor */
	for (counter = 0; counter < exec->segments_no; {
		exec->segments[counter].data =
				malloc(sizeof(struct pageEvidence));
		if (exec->segments[counter].data == NULL) {
			perror("Eroare alocare memorie segment data!\n");
			exit(EXIT_FAILURE);
		}
		counter++;
	}

	so_start_exec(exec, argv);

	return 0;
}
