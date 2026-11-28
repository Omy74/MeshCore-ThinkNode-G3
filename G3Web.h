#pragma once

// Interfaccia web per ThinkNode G3 (Wi-Fi + HTTP). Implementazione in G3Web.cpp.
// Volutamente minimale: nessuna dipendenza da MeshCore, cosi' le librerie web
// restano confinate in un solo file.

#include <stdint.h>

// Ritorna la password admin corrente del repeater.
typedef const char* (*G3WebPasswordFn)();
// Esegue un comando CLI del repeater (come dalla console seriale).
// sender_timestamp = 0 solo per i comandi di sola lettura scelti dal firmware;
// i comandi che arrivano dalla pagina web usano sempre un valore diverso da 0.
typedef void (*G3WebExecFn)(uint32_t sender_timestamp, char* command, char* reply);

void g3web_start(G3WebPasswordFn password, G3WebExecFn exec);
void g3web_poll();
// Memorizza il nome di un nodo (primi 4 byte della chiave pubblica) per mostrarlo tra i vicini.
void g3web_note_name(const uint8_t* pubkey, const char* name);
