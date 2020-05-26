/*
 * Loader Implementation
 *
 * 2018, Operating Systems
 */

#include <stdio.h>
#include <Windows.h>

#define DLL_EXPORTS
#include "loader.h"
#include "exec_parser.h"

/* Handler vechi */
static PVECTORED_EXCEPTION_HANDLER old_handler;
/* Handler nou */
static PVECTORED_EXCEPTION_HANDLER new_handler;
/* File-descriptor-ul fisierului */
HANDLE hFile;
/* Dimensiunea unei pagini */
static DWORD dwPageSize = 0x10000;

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
	unsigned long *pagesVect;
	unsigned long pageCount;
};

/* Handler-ul nou pentru tratarea semnalului SIGSEGV */
static LONG CALLBACK segv_handler(PEXCEPTION_POINTERS ExceptionInfo)
{
	/* Adrese */
	ULONG_PTR addr;
	ULONG_PTR startAddr;
	ULONG_PTR endAddr;
	void *mmapAddr;
	/* Iteratori */
	long counter = 0;
	unsigned long pageIter = 0;
	/* Variabile de verificare */
	DWORD old, rc, dwRet;
	int already_mapped = 0, bytesRead;
	/* Informatii ale paginilor */
	unsigned long pageIndex, page_offset;
	/* Dimensiuni */
	unsigned int mem_size, file_size;
	/* Structuri */
	struct pageEvidence *pgEv;

	/* Daca se face acces invalid la memorie sau se primeste
	 * un alt semnal in afara de SIGSEGV, chemam handler-ul
	 * vechi, conform cerintei din tema.
	 */
	if (ExceptionInfo->ExceptionRecord->ExceptionCode !=
		EXCEPTION_ACCESS_VIOLATION)
		return EXCEPTION_CONTINUE_SEARCH;

	/* Adresa page fault-ului */
	addr = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];

	/* Iteram prin toate segmentele */
	for (counter = 0; counter < exec->segments_no; counter++) {

		/* Retinem adresa de start a segmentului */
		startAddr = exec->segments[counter].vaddr;

		/* Retinem dimesiunea spatiului de adrese */
		mem_size = exec->segments[counter].mem_size;

		/* Retinem adresa de sfarsit a segmentului */
		endAddr = (startAddr + mem_size);

		/* Retinem datele(custom) din segment */
		pgEv = exec->segments[counter].data;

		/* Alocam spatiu pentru vectorul de pagini al structurii
		 * in cazul in care acesta nu a fost alocat deja. Operatia
		 * are loc o singura data, si anume la prima iteratie.
		 */
		if (pgEv->pagesVect == NULL) {
			pgEv->pagesVect = malloc((mem_size / dwPageSize + 1)
							 * sizeof(int));
			pgEv->pageCount = 0;
			if (pgEv->pagesVect == NULL) {
				perror("Eroare alocare pagina!\n");
				exit(EXIT_FAILURE);
			}
		}

		/* Verificam daca adresa pe care o cautam se afla in
		 * intervalul segmentului curent. Daca nu, se apeleaza
		 * handler-ul vechi, iar apoi se trece la urmatorul segment.
		 */
		if (addr <= endAddr && addr >= startAddr) {
			/* Obtinem indexul paginii care a dat page fault */
			pageIndex = (addr - startAddr) / dwPageSize;

			/* Retinem dimensiunea fisierului */
			file_size = exec->segments[counter].file_size;

			/* Retinem offset-ul paginii */
			page_offset = exec->segments[counter].offset
						+ pageIndex * dwPageSize;

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
			if (already_mapped == 1)
				return EXCEPTION_CONTINUE_SEARCH;

			/* Mapam pagina */
			mmapAddr = VirtualAlloc((LPVOID)
					(startAddr + pageIndex * dwPageSize),
						dwPageSize,
						MEM_COMMIT | MEM_RESERVE,
						PAGE_READWRITE);
			if (mmapAddr == NULL) {
				perror("Eroare la maparea paginii!\n");
				exit(EXIT_FAILURE);
			}

			/* Citim din fisier, in functie de dimensiunea lui,
			 * in concordanta cu adresele/indexul paginilor
			 */
			if (file_size > pageIndex * dwPageSize) {
				if (file_size < (pageIndex + 1) * dwPageSize) {
					rc = SetFilePointer(
						hFile,
						page_offset,
						NULL,
						FILE_BEGIN
					);
					dwRet = ReadFile(
						hFile,
						mmapAddr,
				file_size - pageIndex * dwPageSize,
						&bytesRead,
						NULL
					);

					if (bytesRead == -1) {
						perror("Eroare1 read!\n");
						exit(EXIT_FAILURE);
					}
				} else {
					rc = SetFilePointer(
						hFile,
						page_offset,
						NULL,
						FILE_BEGIN
					);
					dwRet = ReadFile(
						hFile,
						mmapAddr,
						dwPageSize,
						&bytesRead,
						NULL
					);

					if (bytesRead == -1) {
						perror("Eroare2 read!\n");
						exit(EXIT_FAILURE);
					}
				}
			}

			/* Setam permisiunile paginii */
			rc = VirtualProtect(mmapAddr, dwPageSize,
				exec->segments[counter].perm, &old);
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
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

	/* Daca adresa nu s-a gasit in niciun segment,
	 * folosim handler-ul vechi
	 */
	return EXCEPTION_CONTINUE_SEARCH;
}


/* Initializam handler-ul nou pentru tratarea semnalului SIGSEGV*/
int so_init_loader(void)
{

	new_handler = AddVectoredExceptionHandler(1, segv_handler);
	if (new_handler < 0) {
		perror("Eroare new_handler\n");
		exit(EXIT_FAILURE);
	}
	return 0;
}

static HANDLE open(const char *filename, DWORD flag)
{
	HANDLE hFile;

	hFile = CreateFile(
			filename,
			FILE_READ_DATA | FILE_WRITE_DATA,
			FILE_SHARE_READ,
			NULL,
			flag,
			FILE_ATTRIBUTE_NORMAL,
			NULL);
	if (hFile == INVALID_HANDLE_VALUE) {
		printf("Eroare CreateFile\n");
		exit(EXIT_FAILURE);
	}
	return hFile;
}

int so_execute(char *path, char *argv[])
{
	int counter;

	/* Parsam executabilul */
	exec = so_parse_exec(path);
	if (exec == NULL) {
		perror("Eroare so_parse_exec!\n");
		exit(EXIT_FAILURE);
	}

	/* Deschidem fisierul ca sa copiem informatii
	 * din memorie
	 */
	hFile = open(path, OPEN_EXISTING);
	if (!hFile) {
		perror("Eroare deschidere fisier!\n");
		exit(EXIT_FAILURE);
	}

	/* Adaptarea permisiunilor pe Windows */
	for (counter = 0; counter < exec->segments_no; ++counter) {

		if (exec->segments[counter].perm & PERM_R &&
			exec->segments[counter].perm & PERM_W &&
			exec->segments[counter].perm & PERM_X) {
			exec->segments[counter].perm = PAGE_EXECUTE_READWRITE;
		} else if (exec->segments[counter].perm & PERM_R &&
			exec->segments[counter].perm & PERM_X) {
			exec->segments[counter].perm = PAGE_EXECUTE_READ;
		} else if (exec->segments[counter].perm & PERM_R &&
				exec->segments[counter].perm & PERM_W) {
			exec->segments[counter].perm = PAGE_READWRITE;
		} else if (exec->segments[counter].perm & PERM_R)
			exec->segments[counter].perm = PAGE_READONLY;
	}

	/* Initializam structura de evidenta a paginilor */
	for (counter = 0; counter < exec->segments_no;) {

		exec->segments[counter].data =
				calloc(1, sizeof(struct pageEvidence));
		((struct pageEvidence *)
			exec->segments[counter].data)->pageCount = 0;

		if (exec->segments[counter].data == NULL) {
			perror("Eroare alocare memorie segment data!\n");
			exit(EXIT_FAILURE);
		}
		counter++;
	}

	so_start_exec(exec, argv);

	return 0;
}

