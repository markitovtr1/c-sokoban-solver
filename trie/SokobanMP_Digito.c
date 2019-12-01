#include "../common/common.h"
#include "../common/sort.h"
#include "../common/structures.h"
#include "../common/util.h"
#include "trie.h"
#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Array para o espalhamento dos IDs.
IdTrie *mainId;

State **lastMainState;

// Função que procura o id na lista
unsigned char findId(State *s) {
	// Apontamos para a mainTrie
	IdTrie *tempTrie = mainId;
	unsigned short tempValue = 0;
	unsigned char found = 0;

	// Para cada caixa:
	for (short i = 0; i < s->boxes; i++) {
		if (s->posBoxes[i] > 100) {
#pragma critical(part1)
			{
				if (!tempTrie->idLeafs[s->posBoxes[i] / 100]) {
					tempTrie->idLeafs[s->posBoxes[i] / 100] = new_trie();
					found = 1;
				}
				tempTrie = tempTrie->idLeafs[s->posBoxes[i] / 100];
			}
		}
		tempValue = (s->posBoxes[i] / 10);
#pragma critical(part2)
		{
			if (!tempTrie->idLeafs[tempValue % 10]) {
				tempTrie->idLeafs[tempValue % 10] = new_trie();
				found = 1;
			}
			tempTrie = tempTrie->idLeafs[tempValue % 10];
		}

#pragma critical(part3)
		{
			if (!tempTrie->idLeafs[s->posBoxes[i] - tempValue * 10]) {
				tempTrie->idLeafs[s->posBoxes[i] - tempValue * 10] = new_trie();
				found = 1;
			}
			tempTrie = tempTrie->idLeafs[s->posBoxes[i] - tempValue * 10];
		}
	}
#pragma critical(part4)
	{
		if (s->posPlayer > 100) {
			if (!tempTrie->idLeafs[s->posPlayer / 100]) {
				tempTrie->idLeafs[s->posPlayer / 100] = new_trie();
				found = 1;
			}
			tempTrie = tempTrie->idLeafs[s->posPlayer / 100];
		}
		tempValue = (s->posPlayer / 10);
	}

#pragma critical(part5)
	{
		if (!tempTrie->idLeafs[tempValue % 10]) {
			tempTrie->idLeafs[tempValue % 10] = new_trie();
			found = 1;
		}
		tempTrie = tempTrie->idLeafs[tempValue % 10];
	}

#pragma critical(part6)
	{
		if (!tempTrie->idLeafs[s->posPlayer - tempValue * 10]) {
			tempTrie->idLeafs[s->posPlayer - tempValue * 10] = new_trie();
			found = 1;
		}
	}
	return found;
}

//-------------------------------------------------------------------

// Função de Hash para pegar o ID do Estado
unsigned char getStateId(State *s) {
	// Fazemos um sort pois a ordem das caixas não pode importar
	quickSort(s->posBoxes, 0, s->boxes - 1);
	return findId(s) == 0;
}

int main(int argc, char *argv[]) {
	struct timespec before, after;
	time_t nSeconds;

	// Começamos a contagem de tempo.
	clock_gettime(CLOCK_REALTIME, &before);

	// Começamos um contador para a lista principal
	unsigned int mainStates = 1;

	// Criamos espaço para uma variável compartilhada que verifica se foi
	// encontrado uma solução por algum dos threads
	unsigned char *solution = malloc(1);
	*solution = 0;

	// Criamos espaço para a raiz da lista principal
	State *root = malloc(sizeof(State));
	root->nextState = NULL;

	// Criamos um ponteiro temporário que irá ser movido
	State *s = malloc(sizeof(State));

	// Ponteiro para o último estado principal é inicializado.
	lastMainState = malloc(sizeof(State *));
	*lastMainState = NULL;

	// Ponteiro para a raiz da trie de Ids
	mainId = malloc(sizeof(IdTrie));
	memset(mainId->idLeafs, 0, 10 * sizeof(IdTrie *));

	// Constroi o primeiro estado, sequencialmente
	buildMap(root, argv[1]);
	getStateId(root);

	// Quantidade de threads solicitados
	int threads = strtol(argv[2], NULL, 10);

	// Pediremos que main faça NUM_MAIN_STATES estados para cada thread
	unsigned int numStates = NUM_MAIN_STATES * threads;

	while (mainStates < numStates) {
		for (int i = 0; i < 4; i++) {
			// Pra cada direção, nós copiamos o estado inicial
			copyState(root, s);
			if (movePlayer(s, i, getStateId) != 0) {
				/*movePlayer retorna 0 se não foi possível mover, seja por uma
				caixa sendo empurrada numa parede,
				seja por estarmos andando de cara na parede*/
				mainStates++;
				if (insertState(root, s, lastMainState)) {
					// Se ele entrou aqui, quer dizer que, durante a inserção,
					// foi notado que ele é um estado final.
					printPath(s);
					*solution = 1;
					// Finalizamos a contagem de tempo.
					clock_gettime(CLOCK_REALTIME, &after);

					// Calcula o tempo passado em microssegundos.
					nSeconds = after.tv_nsec - before.tv_nsec +
					           (after.tv_sec - before.tv_sec) * NANOS;

					printf("Achei sem threads: %lu ns - %lf s\n", nSeconds,
					       (double)nSeconds / NANOS);
					return 0;
				}
			}
		}
		// Movemos root, colocando root como próximo estado
		popState(&root, &root);
	}
	// Chegando aqui, temos uma lista ligada à root com n<=4 estados.
	/*
	   A estratégia aqui é: criar n threads, e sequencialmente cada um pega um
	   estado da lista para si. Abriremos estes estados, agora paralelamente, em
	   cada thread, criando uma lista ligada parcial. Cada thread procedirá para
	   criar SIZE_THREAD_LIST estados, e então conectá-lo á lista principal.
	*/

	// root, lastMainState e solution serão compartilhados, todo resto é
	// declarado internamente e portanto, são privados.
#pragma omp parallel num_threads(threads) shared(root, lastMainState, solution)
	{
		// threadRoot será a raiz da lista ligada temporária de cada thread
		State *threadRoot = NULL;

		// Estado para ser movido
		State *s;

		// Variável de condição que nos diz se devemos pegar um estado da lista
		// principal ou não
		unsigned char popMainList = 1;

		// Quantidade de estados ativos no thread
		unsigned int activeThreadStates = 0;

		// Criamos espaço para o estado temporário móvel
		s = malloc(sizeof(State));

		// Criamos espaço para o ponteiro para o último estado presente neste
		// thread
		State **lastThreadState;
		lastThreadState = malloc(sizeof(State *));
		(*lastThreadState) = NULL;

		// Enquanto não foi encontrado uma solução por nenhum thread
		while (!(*solution)) {

			// Se a variável de condição foi 1, devemos pegar um estado da lista
			// principal. Isto só acontecerá caso chegamos no limite estipulado
			// para cada thread, ou caso esta seja a primeira iteração de cada
			// thread
			if (popMainList) {
				// Esta região deve ser crítica, pois estamos mexendo com a
				// lista principal (e portante shared)
#pragma omp critical(popMerge)
				popState(&root, &threadRoot);

				// Limpamos o popMainList
				popMainList = 0;
			}

			// Pra cada direção, iremos mover o estado, e depois adicionar na
			// nossa lista temporária.
			for (int i = 0; i < 4 && !(*solution); i++) {
				copyState(threadRoot, s);
				if (movePlayer(s, i, getStateId) != 0) {
					// Entrou aqui, quer dizer que ele conseguiu se mover, ou
					// seja, era um movimento válido.
					activeThreadStates++;
					if (insertState(threadRoot, s, lastThreadState)) {
						// Entrou aqui quer dizer que o estado era final, de
						// acordo com a definição de estado final.
						printPath(s);
						*solution = 1;
					}
				}
			}
			// Chegado aqui, exploramos as quatro direções.

			// Tentaremos criar uma lista de pelo menos SIZE_THREAD_LIST
			// elementos antes de adicionar à lista principal. Caso não
			// conseguimos estados suficientes, activeThreadStates = -1, todos
			// os nós que pegamos eram inúteis. Isso significa que precisamos
			// pegar novos nós da lista principal
			if (activeThreadStates < SIZE_THREAD_LIST &&
			    activeThreadStates > 0) {
				// Desempilhamos mais um, agora da nossa lista temporária, pois
				// não passamos da quantidade necessária
				popState(&threadRoot, &threadRoot);
				activeThreadStates--;
			} else {
				if (activeThreadStates > 0 && !(*solution)) {
					// há lista para empilhar
					// Como no pop acima, esta região é critica (e de mesmo nome
					// do pop) pois mexe com a lista principal
#pragma omp critical(popMerge)
					mergeLinkedLists(&threadRoot, lastThreadState, &root,
					                 lastMainState);

					// Não há mais estados ativos no thread
					activeThreadStates = 0;
				} /*if*/
				// Ordenamos que se retire da lista principal mais um nó para
				// ser expandido
				popMainList = 1;
			} /*else*/

		} /*while*/

	} /*pragma*/

	// Finalizamos a contagem de tempo.
	clock_gettime(CLOCK_REALTIME, &after);

	// Calcula o tempo passado em microssegundos.
	nSeconds =
	    after.tv_nsec - before.tv_nsec + (after.tv_sec - before.tv_sec) * NANOS;

	printf("Tempo total: %lu ns - %lf s\n", nSeconds, (double)nSeconds / NANOS);

	return 0;
}