#include <SPI.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266WiFi.h>
#include <DYE_Fingerprint.h>
#include <SoftwareSerial.h>
#include <AmazonIOTClient.h>
#include "ESPAWSImplementations.h"
#include "credentials.h"

#define inductionSignal 16
#define activeLed 0
#define closedLed 14
#define pendingLed 12
#define openLed 13
#define door 1 //ID della porta. Cambiando qeuesta riga e alcuni parametri nel file credentials.h è possibile aggiungere un numero indefinito di porte

SoftwareSerial mySerial(5,4);
DYE_Fingerprint finger = DYE_Fingerprint(&mySerial);
int id=-1;
int internalStatus=0; //0 closed, 1 pending, 2 open
long timeRequest; //stores timestamp of when the request is made,  minute timeout, then the status is restored to closed
EspHttpClient httpClient;
EspDateTimeProvider dateTimeProvider;
AmazonIOTClient iotClient;
ActionError actionError;

void setup() {
  pinMode(inductionSignal, INPUT); //imposto la porta per accorgermi quando il dito si avvicina al sensore e attivarlo
  pinMode(activeLed, OUTPUT); //led che mi dirà se il sistema è attivo
  pinMode(closedLed, OUTPUT); //led che segnala che la porta è chiusa e il sistema è pronto ad accettare richieste
  pinMode(pendingLed, OUTPUT); //led che segnala che è stata inoltrata una richiesta
  pinMode(openLed, OUTPUT); //led che segnala che la porta è aperta
  digitalWrite(activeLed, LOW); //Disattivo il led di stato
  Serial.begin(115200);
  setup_wifi(); //connessione al wifi
  finger.begin(57600); //Inizializzo il sensore di impronte, settando la velocità di trasferimento dati
    
  if (finger.verifyPassword()) { //accoppio il lettore alla board
    Serial.println("Fingerprint sensor found!");
  }
  else{
    Serial.println("Fingerprint sensor not found: Sorry :(");
    while (1) { //Se ci sono problemi con l'accoppiamento del sensore faccio lampeggiare il led bianco di stato. Siccome è comune che si spostino dei cavi, mi rendo immediatamente conto del problema
      digitalWrite(activeLed, HIGH); //Attivo il led di stato
      delay(2000);
      digitalWrite(activeLed, LOW); //Disattivo il led di stato
      delay(1000);
    }
  }

    //setto i parametri di AWS
    iotClient.setAWSRegion(aws_region);
    iotClient.setAWSEndpoint("amazonaws.com");
    iotClient.setAWSDomain(broker);
    iotClient.setAWSPath(aws_topic);
    iotClient.setAWSKeyID(aws_key);
    iotClient.setAWSSecretKey(aws_secret);
    iotClient.setHttpClient(&httpClient);
    iotClient.setDateTimeProvider(&dateTimeProvider);
  
  digitalWrite(activeLed, HIGH); //Attivo il led di stato
  statusLed(); //Attivo il led che indica porta chiusa
}

void loop(){
  
  if(digitalRead(inductionSignal)==1 && internalStatus==0){ //Se il sensore si accorge che un dito è vicino e che la porta è chiusa e non esiste un tentativo d'accesso ancora in corso
    id=controlFingerPrint(); //avvio la funzione di scansione
    if(id!=-1){ //Se il dito viene riconosciuto
           internalStatus=1; //modifico lo stato della porta
           timeRequest=millis(); //Annoto l'ora in cui è stato riconosciuto il dito
           statusLed(); //Modifico il led attivo
        }
    }
  if(internalStatus==1){ //Se esiste un tentativo di accesso
      const char* result= iotClient.get_shadow(actionError); //Scarico lo shadow
      String s = String(result);
      if(s.indexOf("open")>0){ //Se lo shadow indica che la porta deve essere aperta
        internalStatus=2; //cambio lo status
        timeRequest=millis(); //registro il momento
        statusLed();  //modifico la configurazione per aprire la porta (accendere il led verde)
      }
      else{ //altrimenti
        delay(2500);  //aspetto due secondi e mezzo (per non spammare AWS di tentativi)
      }
      
  }
  
  timeout(); // gestisco la chiusura temporizzata della porta
}


int controlFingerPrint() { //riconosco l'impronta in locale
  uint8_t p = finger.getImage();
  
  if (p != FINGERPRINT_OK){
    return -1;
  }

  p = finger.image2Tz();
  
  if (p != FINGERPRINT_OK){ 
    return -1;
  }

  p = finger.fingerFastSearch();
  if (p != FINGERPRINT_OK){
    return -1;
  }
  
  return finger.fingerID;
}

void setup_wifi() {
  delay(10);
  Serial.println("Attempting WIFI connection...");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED){
    delay(500);
  }
  Serial.println("WiFi connected");
}

void timeout(){ //Gestisco la chiusura temporizzata della porta o l'annullamento della rischiesta
  if(internalStatus!=0 && millis()-timeRequest>=60000){ //Se la porta non è chiusa ed è passato più di un minuto dall' ultimo cambio di stato
    internalStatus=0; //chiudo la porta
    statusLed(); //modifico la configurazione dei led
  }
}

void statusLed(){ //Funzione che si occupa di modificare lo stato dei led
  switch(internalStatus){ //in base allo stato
    case 0: //accende la luce rossa e spegne le altre
      digitalWrite(closedLed, HIGH);
      digitalWrite(pendingLed, LOW);
      digitalWrite(openLed, LOW);
      break;
    case 1: //accende la luce gialla e spegne le altre
      digitalWrite(closedLed, LOW);
      digitalWrite(pendingLed, HIGH);
      digitalWrite(openLed, LOW);
      break;
    case 2://accende la luce verde e spegne le altre
      digitalWrite(closedLed, LOW);
      digitalWrite(pendingLed, LOW);
      digitalWrite(openLed, HIGH);
      break;
  }
    sendmessage(); //notifica il cambiamento ad AWS
  
}

void sendmessage() { //Funzione che si occupa di notificare lo stato ad AWS
    String s;
    int rc;
    switch(internalStatus){
      case 0: //porta chiusa
        s="{\"state\":{\"reported\":{\"status\": \"closed\", \"idPorta\": " + String(door) + ", \"id\": -1}, \"desired\":{\"status\": \"closed\", \"idPorta\": " + String(door) + ", \"id\": -1}}}"; 
        break;
      case 1: //richiesta pending
        s="{\"state\":{\"reported\":{\"status\": \"pending\", \"idPorta\": " + String(door) + ", \"id\": " + String(id) + "}, \"desired\":{\"status\": \"pending\", \"idPorta\": " + String(door) + ", \"id\": " + String(id) + "}}}";
        break;
      case 2: //porta aperta
        s="{\"state\":{\"reported\":{\"status\": \"open\", \"idPorta\": " + String(door) + ", \"id\": -1}, \"desired\":{\"status\": \"open\", \"idPorta\": " + String(door) + ", \"id\": -1}}}";
        break;
    }

    char buf[s.length() +1];
    s.toCharArray(buf, s.length()+1);
    iotClient.update_shadow(buf, actionError);
   
}

