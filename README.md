# Repetor Crossband VHF-UHF

### Proiect de comunicatii radio portabil bazat pe ESP32 si module SA

## 1. Scop

Dispozitivul a fost conceput pentru a facilita comunicatia radio intre echipe sau persoane aflate
in locatii fara vizibilitate directa, unde propagarea radio in linie dreapta este obstructionata de
relief, cladiri sau vegetatie. Proiectul include contributii personale in domeniul proiectarii
hardware si al programarii sistemelor cu microcontrollere.

## 2. Utilitate Practica

Repetorul crossband preia semnalele dintr-o banda de frecventa (VHF sau UHF) si le
retransmite in banda corespunzatoare (UHF sau VHF), extinzand astfel raza de comunicatie
si permitand interoperabilitatea intre echipamente care opereaza pe benzi diferite.

Aplicatii practice tipice:

- Operatiuni de urgenta in zone calamitate unde infrastructura de comunicatii lipseste
- Competitii sportive in teren accidentat (trasee montane, orientare)
- Expeditii si activitati de turism in zone izolate
- Radioamatorism si experimentare tehnica

## 3. Scopul si Obiectivele Proiectului

Obiectivul principal al proiectului este realizarea unui repetor crossband compact, portabil si
accesibil ca pret, care sa poata inlocui solutiile industriale costisitoare.

Obiective specifice:

- Implementarea functiei de repetare crossband VHF ↔ UHF cu minimizarea
    interferentelor radio
- Controlul complet al parametrilor radio (frecventa, CTCSS, squelch) prin interfata
    web
- Monitorizarea telemetriei in timp real (tensiune baterie, stare canale)
- Alimentare flexibila – compatibila cu baterii de acumulatoare (Pb, Li, etc.), surse de
    alimentare de la retea sau panouri solare
- Raza de acoperire de minimum 10 km in conditii favorabile de propagare

## 4. Descriere Generala

Repetorul crossband este realizat in jurul a doua module RF de tip SA818, controlate de un
microcontroller ESP32. Alegerea arhitecturii crossband (VHF in, UHF out si invers) elimina
necesitatea unui duplexor RF de inalta performanta si reduce semnificativ interferenta intre
canalul de receptie si cel de emisie.

ESP32 asigura controlul complet al modulelor radio prin interfata UART, gestioneaza logica
de repetare (detectie squelch, activare PTT), gazduieste un server web local pentru
configurare si afiseaza telemetria in timp real.


Dispozitivul poate fi alimentat de la orice sursa disponibila: baterie interna reincarcabila, sursa
externa 5–12V sau panou solar, ceea ce il face ideal pentru functionare autonoma.

## 5. Componenta Hardware

### 5.1 Lista componentelor principale

```
Nr. Componenta Specificatie Rol
```
```
1 SA818-V VHF 134–174 MHz, 1W Modul RF canal VHF
(receptie/emisie)
```
```
2 SA818-U UHF 400–480 MHz, 1W Modul RF canal UHF
(receptie/emisie)
```
```
3 ESP32 DevKit Dual-core 240 MHz, Wi-
Fi/BT
```
```
Microcontroller principal, server
web
```
```
4 Filtru diplexor Home-made, 2×
Butterworth 5 poli
```
```
Separare VHF/UHF pe antena
comuna
5 Antena dual-band VHF/UHF, 50Ω Radiator semnal VHF si UHF
```
```
6 Circuit monitorizare baterie^ Divizor rezistiv + ADC
ESP
```
```
Masurare tensiune baterie
```
### 5.2 Module RF SA

Cele doua module SA818 (varianta VHF si varianta UHF) reprezinta inima sistemului RF.
Fiecare modul integreaza un transceiver complet (receptor superheterodina + emitator FM)
controlabil prin comenzi AT via UART. Parametrii programabili includ frecventa de emisie si
receptie, nivelul de squelch, tonul CTCSS/DCS si puterea de iesire (1W / 0.5W).

Asignarea pinilor ESP32 pentru comunicatia cu modulele SA818:

```
Semnal SA818-V (VHF) SA818-U (UHF)
```
```
UART TX (ESP→SA818) GPIO 1 6 GPIO 14
UART RX (SA818→ESP) GPIO 1 7 GPIO 12
```
```
PTT (Push-To-Talk) GPIO 26 GPIO 1 3
SQ (Squelch detect) GPIO 27 GPIO 3 3
```
```
Ton 1kHz PWM GPIO 25 GPIO 25
```
## 6. Filtrul Diplexor Home-Made

Filtrul diplexor este componenta cheie care permite conectarea ambelor module SA818 la o
singura antena dual-band VHF/UHF, fara interferenta intre benzile de lucru. A fost proiectat,
simulat si construit manual.

### 6.1 Metodologie de proiectare

Filtrul a fost calculat si simulat cu ajutorul programului RFSIM99, care permite modelarea si
analiza comportarii circuitelor RF pasive. Structura adoptata consta in alaturarea a doua filtre
independente:


- Filtru trece-jos (LPF) cu 5 poli Butterworth – pentru banda VHF (≤ 200 MHz), asigura
    trecerea semnalelor VHF si rejectia celor UHF
- Filtru trece-sus (HPF) cu 5 poli Butterworth – pentru banda UHF (≥ 300 MHz),
    asigura trecerea semnalelor UHF si rejectia celor VHF

Frecventele de taiere au fost alese astfel incat banda de trecere sa prezinte atenuare minima,
iar banda de rejectie sa prezinte atenuare maxima, asigurand izolare suficienta intre porturile
VHF si UHF.

```
Fig 1. Schema LPF si HPF
```
```
Fig 2. Simulari caracteristicilor filtrelor LPF si HPF
```
### 6.2 Constructie

Componentele filtrului (bobine si condensatoare) au fost dimensionate pe baza tabelelor
Butterworth si verificate prin masuratori cu analizorul de retea NanoVNA. Montajul a fost
realizat pe o placa de circuit imprimat dedicata, cu atentie la minimizarea inductantelor si
capacitatilor parazite.

```
Fig 3. Caracteristicile amplitudine-frecventa pentru LPF si HPF
```

Fig 4. Schema electrica a repetorului


## 7. Schema Bloc si Electrica

Schema electrica completa a fost realizata in EasyEDA si include:

- Modulul SA818-V (VHF) cu circuitul de interfatare audio, circuitul de interfatare al
    semnalelor SQ/PTT
- Modulul SA818-U (UHF) cu configuratie simetrica fata de modulul VHF
- ESP32 DevKit v1 – microcontroller central, conectat la ambele module SA818 prin
    UART1 si UART
- Circuitul de alimentare cu monitorizare tensiune baterie
- Filtrul diplexor conectat intre cele doua module SA818 si antena comuna
- Conectori pentru antena, baterie/alimentare externa si periferice

Schema contine de asemenea circuite de conditionare a semnalului audio (reteaua RLC
pentru AF), circuitul de generare ton CTCSS si etajele de amplificare/atenuare MIC/AF pentru
adaptarea nivelurilor de semnal intre modulele SA818.

```
Fig 5. Montaj repetor
```
## 8. Componenta Software

### 8.1 Arhitectura firmware

Firmware-ul ESP32 este scris in C++ si ruleaza sub framework-ul Arduino/ESP-IDF. Acesta
implementeaza urmatoarele functionalitati principale:

- Initializarea si configurarea modulelor SA818 la pornire (frecvente, squelch, CTCSS,
    putere)
- Loop-ul ce asigura functia de repetor: detectia squelch activ pe unul dintre module si
    activarea PTT pe modulul opus


- Server web local (Wi-Fi AP sau STA) pentru configurare parametri si vizualizare
    telemetrie
- Monitorizarea tensiunii bateriei prin ADC si transmiterea valorii catre interfata web
- Generare ton de identificare 1kHz direct pe GPIO 25 prin DAC-ul intern al ESP32,
    fara modul audio extern

### 8.2 Control prin platforma web

Repetorul poate fi controlat in totalitate prin intermediul platformei web integrate, accesibila
de pe orice dispozitiv conectat la reteaua Wi-Fi a ESP32. Prin aceasta interfata sunt
disponibile:

- Setarea frecventelor de emisie si receptie pentru ambele canale (VHF si UHF)
- Configurarea tonului CTCSS/DCS si a nivelului de squelch
- Comutarea puterii de iesire (High 1W / Low 0.5W)
- Vizualizarea telemetriei in timp real: tensiune baterie, temperatura, stare squelch
- Reset software si salvarea configuratiei in memoria flash NVS

## 9. Rezultate si Performante

### 9.1 Raza de acoperire

Experimentele cu repetorul au fost facute folosind benzile de frecventa VHF 144-146 MHz si
UHF 430 - 4 40 MHz alocate radioamatorilor. Ca statii mobile au fost folosite doua tranceivere
portabile TYT MD390 si LANCHONLH HG-UV98.

Antena dual-band folosita la repetor este NAGOYA NA-707 cu castig 3dBi.

In conditii de teren favorabil (deal cu vizibilitate extinsa), dispozitivul a demonstrat o raza de
acoperire de aproximativ 10 km pe ambele benzi, cu antena de tip dual-band verticala.

Performanta este limitata in principal de puterea de iesire a modulelor SA818 (1W) si de
castigul antenei utilizate.

### 9.2 Filtrul diplexor – rezultate simulare

```
Parametru LPF (VHF) HPF (UHF)
```
```
Tip filtru Butterworth 5 poli Butterworth 5 poli
```
```
Frecventa de taiere ~200 MHz ~300 MHz
Atenuare in banda de trecere < 1 dB < 1 dB
```
```
Atenuare in banda de rejectie > 40 dB > 40 dB
Impedanta caracteristica 50 Ω 50 Ω
```
### 9.3 Consum energetic si autonomie

Consumul total al sistemului in modul de asteptare (fara emisie activa) este de aproximativ
200 – 300 mA la 5V (inclusiv ESP32, OLED si modulele SA818 in modul RX). In emisie activa
(un canal TX), consumul creste la aproximativ 600–800 mA. Cu o baterie LiPo de 3000 mAh,
autonomia estimata in utilizare normala (ciclu 30% TX / 70% RX) depaseste 8 ore.


## 10. Comparare cu Alte Solutii

```
Criteriu Repetoare industriale Acest proiect
Cost 500 – 5000 EUR < 50 EUR
```
```
Greutate 2 – 20 kg < 500 g
```
```
Dimensiuni Rack 19" Cutie compacta
Consum 10 – 50 W 2 – 4 W
```
```
Configurare Software dedicat / tehnicieni Interfata web accesibila
Portabilitate Stationare/mobile cu efort Portabil, folosit cu rucsacul
```
```
Raza acoperire 20 – 100 km (putere mare) ~10 km (suficient pentru scopul
propus)
```
## 11. Abilitati Utilizate

Realizarea acestui proiect a implicat un spectru larg de competente tehnice si practice:

- Proiectare schema electronica (EasyEDA) si layout PCB
- Lipire componente SMD si prin-gaura, asamblare mecanic
- Proiectare si simulare circuite RF (RFSIM99) – filtre Butterworth LPF/HPF
- Programare in C++ pentru sisteme incorporate (ESP32, framework Arduino/ESP-
    IDF)
- Dezvoltare server web embedded (HTML/CSS/JS servit din flash-ul ESP32)
- Configurare si testare module radio SA818 prin comenzi AT
- Masuratori RF si verificare performanta filtrului diplexor
- Testare in teren – expeditii radio pentru validarea performantelor

## 12. Concluzii

Repetorul crossband VHF-UHF proiectat si realizat in cadrul acestui proiect reprezinta o
solutie tehnica eleganta, portabila si accesibila pentru extinderea comunicatiilor radio in zone
fara acoperire directa. Principalele avantaje ale solutiei sunt simplitatea constructiva, costul
redus si flexibilitatea de configurare prin interfata web.

Dispozitivul a fost validat in teren, demonstrand o raza de acoperire de aproximativ 10 km,
suficienta pentru scenariile vizate: urgente, competitii sportive si expeditii. Comparativ cu
solutiile industriale existente, repetorul de fata ofera un raport performanta/cost/portabilitate
net superior pentru aplicatii mobile.

Directii de dezvoltare ulterioare:

- Cresterea puterii de emisie prin utilizarea unui etaj de amplificare RF extern
- Integrarea unui GPS pentru transmitere pozitie (APRS)
- Adaugarea unui modul LoRa pentru comunicatie de date de lunga distanta
- Implementarea unui protocol de comanda la distanta prin DTMF sau radio


