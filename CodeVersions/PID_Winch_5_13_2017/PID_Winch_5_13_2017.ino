#include <Statistic.h> //Libary imported for easy statistic calculations
#include <Servo.h>  //Library used for servo control; Documentation - https://www.arduino.cc/en/Reference/Servo
#include <Encoder.h> //Library used for encoder function
#include <Timer.h> //Library for using timer functions
#include <PID_v1.h> //Custom PID library
#include <String.h> //Library for string functions

//Winch control version for use with the XLX green ESC
//Note: speeds and other variables need to be adjusted
//Note: code only checks for HES output when within slowing distance. May need to be changed.

#define codeVersion "5_13_2017"

#define DEBUG //Uncomment to print debugging information via serial
//#define TEST //Uncomment to run test with fake resistance
#define DOPID //Uncomment to run PID loop
//#define LINEAR  //One these must be defined to determine the ramping type 
#define SINUSOID  //One of these must be defined to determine the ramping type


Statistic myStats; //Initializes the statistic library with type myStats


struct Winch_TYPE { //Assigns variables to the object "winch" (Ex, winch.currentSpeed is now a variable, as is winch.prevSpeed)
  double currentSpeed;
  double prevSpeed;
  uint8_t currentDir; // UP,DOWN,STOP defined in enum
  uint8_t prevDir;
  uint8_t prevDesiredDir;
  double prevDesiredSpeed;
} winch; //This is the object name for the above structure


Servo ESC; //Create ESC object
Encoder winchEncoder(3,2); //Create encoder object
Timer statusTimer; //Create a timer for sending status messages
  
enum{ //Assign integer values to each direction
  UP, //starts at 0
  DOWN,
  STOP
};

enum{  //Assign integer values to each state
  CHECK_BUFFER,
  CONTROL_WINCH,
  MAINTAIN
};

#define ENCODER_OPTIMIZE_INTERRUPTS
//Maximum, minimum, and neutral pulse widths in microseconds 
//ESC 1: 1910, 1479, 1048
//ESC 2: 1980, 1534, 1086
//ESC 3 (green XLX 12/1/2016): 2405, 1475, 545  
#define MAX_FORWARD 1956 //Maximum, minimum, and neutral pulse widths in microseconds
#define NEUTRAL 1515
#define MAX_REVERSE 1074
#define REV(x) 3936*x //Converts revolutions into encoder pings

//Speed constants
#define SLOW_DIST 15 //Distance in revolutions from full upright to begin changing winch speed in
#define LIFT_SPEED 25//Speed for lifting the A-frame when finishing a profile
#define MAINTAIN_SPEED 25 //Speed for lifting the A-frame when maintaining
#define FAST_IN_SPEED 50 //Speed for returning fast AND maintaining
#define SLOW_IN_SPEED 25 //Speed for returning slow AND maintaining
#define STOP_SPEED 7 //The speed the winch actually stops due to its wide stop band; this number depends on how the winch was configured
#define SPEED_DIFF 3 //The speed difference to check for to see if bottom has been hit

//Define remote start/stop pins
#define remoteStartPin 5
#define remoteStopPin 6
#define remoteStartLED 7
#define remoteStopLED 8

#define startTime 750 //How long remote start is held HIGH in milliseconds

//Define sensor pins
#define downPin A3//Hall Effect sensor indicating lowered position (no longer used) (digital 17) (S1)
#define downLED 13
#define upPin A4 //Hall Effect sensor indicating upright position (digital 18) (S2)
#define upLED 15

//Define analog pins
#define resistancePin A7 //fishing rod strain gauge resistance (analog pin)

//Define PID tuning parameters (must be >= 0)
//error = desired - input
//output = Kp*error + integral(Ki*error) - Kd*(input - lastinput)
 double Kp = 0; // proportional - reacting to current error
 double Ki = 1; // integral - reacting to error over time
 double Kd = 0; // derivative - reacting to change in error
const int KDir = DIRECT;  // if DIRECT, +INPUT leads to +OUTPUT, if REVERSE, +INPUT leads to -OUTPUT

// resistance calibration settings for the fishing rod
double straightResistance = 810; //ohms - rod straight (no tension) (Daywa Custom Design (Dark Grey) - 800)
const double stopResistance = 0; //ohms - position of rod to send stop command (getting too slack) (should be about 722)
double bentResistance = 720; //ohms - rod bent (full tension) (should be about 620)
double desiredResistance = 763; //ohms - resistance to attempt to maintain at all times
double resistance = 0;  //TEST
double rodMultiplier = 0.85; //Used to set the PID setpoint (85% of the max-min)

// frequency of status messages in ms
#ifdef DEBUG
    int statusFrequency = 250;
#else
    int statusFrequency = 250;
#endif

const double RAMP_TIME = 500; //Time it takes to change speed in milliseconds
const double RAMP_TIME_DOWN = 3000; //Ramp time at beginning of profile to prevent tangling
const double SINFACTOR = 0.5; // factor used in the sinusoidal speed function
const float pi = 3.14159;
uint64_t t0 = 0; //Beginning time for speed change
double speedDifference = 0; //Difference between desired and current speed
int parameters[7];
int incomingByte = 0;

int state = CHECK_BUFFER;
int firstbyte; //First byte to check consistency with RPI
int secondbyte = 0x00; //Second byte to check consistency with RPI
int header = 0;
int upperByte;
int lowerByte;
int uByte;

int checksum; //Used to check data consistency with RpI
int buffSize = 0; //Store the count of bytes recieved by RpI

float speedOut; //variable from MatLab
float speedIn; //variable from MatLab
double speedTry; //Variable to store the PID output
long long depth; //estimated depth based on encoder signal
long long doubleByte; //extra two-byte parameter currently used to set fishing rod resistance
bool motorRunning = false; //Variable for storing motor state
bool depthReached = false; //Tells us if we've hit our target depth
bool halt = false; //Variable for stopping all function in emergency
bool returned = true; //Store location of CTD (cast out still or by boat)
bool dataCorrupted = false; //The status of the data coming from the RPI
bool go = false; //Used to specify if buffer is synced with RPI
bool kickstart = true; //used to kickstart pid for sinusoid
int warningCounter = 0; //variable for counting 
int PIDCounter = 0; //variable for counting
int avgvalue = 0; //variable used for stats functions
bool bottomHit = false; //Tells us if we hit the bottom
float speedAVG = 0.0; //average over x iterations
float speedAVGold = 0.0; //previous average over x iterations
String speedAVGmsg = ""; // long debugging text string with speedAverage data
String bottomMsg = ""; // message for bottom detection


PID rodPID(&resistance, &speedTry, &desiredResistance, Kp, Ki, Kd, KDir); //Create PID object and links


void setup() 
{
    // put your setup code here, to run once:
    Serial1.begin(57600);
    #ifdef DEBUG
      Serial.begin(9600);
    #endif
    rodPID.SetSampleTime(15);
    winch.currentSpeed = 100; //Initialize all struct values to stationary
    winch.prevSpeed = 100;
    winch.currentDir = STOP;
    winch.prevDir = STOP;
    winch.prevDesiredDir = STOP;
    winch.prevDesiredSpeed = 0;
    //Set pin modes
    pinMode(remoteStartPin, OUTPUT);
    pinMode(remoteStopPin, OUTPUT);
    pinMode(remoteStartLED, OUTPUT);
    pinMode(remoteStopLED, OUTPUT);
    pinMode(upLED, OUTPUT);
    pinMode(downLED, OUTPUT);
    pinMode(downPin, INPUT_PULLUP);  // this was for the new push switch instead of Hall Effect but ok for both styles
    pinMode(upPin, INPUT_PULLUP);  // this was for the new push switch instead of Hall Effect but ok for both styles
    //Initialize pin states
    digitalWrite(remoteStartPin, LOW);
    digitalWrite(remoteStopPin, HIGH);
    digitalWrite(remoteStartLED, LOW);
    digitalWrite(remoteStopLED, LOW);
     
    statusTimer.every(statusFrequency, sendStatus); //Send a status message every X milliseconds
    
    ESC.attach(9, MAX_REVERSE, MAX_FORWARD); //Connect ESC to pin 9 with maximum and minimum puse width values
    ESC.writeMicroseconds(NEUTRAL); //Start the winch in neutral
    delay(5000); //Allow ESC to receive neutral signal for proper amount of time
  
    rodPID.SetOutputLimits(-10, 10); //output given by PID controller is constrained (Should be used to change and input to the plant system)
    rodPID.SetMode(AUTOMATIC);
  
      while(atTop() == false) //waits for the A-frame to be all the way up (IN SETUP FUNCTION, IT WILL WAIT TO START PROGRAM UNTIL THIS IS TRUE)
      { 
        changeSpeed(LIFT_SPEED, UP); //if for some reason it isn't we set the speed to go up 
        Serial1.println("Initial lift.");
      }
    changeSpeed(0, STOP); //Once Hall effect is tripped, we stop our motion
    winchEncoder.write(0); //Zero our encoder (Thinks we are at depth "0")
    returned = true; 
    myStats.clear(); //Forces the "myStats" constructor to start with no data
}

void loop() {
  statusTimer.update();
  myStats.clear(); //make sure to clear old data from myStats constructor
  switch(state)
{
    
    case CHECK_BUFFER:

       if(buffSize == 7) //Update parameters if the serial buffer is full (new packet fully received)
         { 
           updateParameters();
           Serial.println(header);
         }
         state = CONTROL_WINCH;
         
       break;
     
    case CONTROL_WINCH:

      if(header == 0xCA)  //measure calibration straight
      {
        avgvalue = 0;
        while(avgvalue == 0) //Keep going until we get a new average value
        {
          for(int i = 0; i < 50; i++) //Loop will count up 50 data points in under 5 secs
          {
              myStats.add(analogRead(resistancePin));          
              delay(100);
          }
            avgvalue = myStats.average() + 1;
            Serial.println(avgvalue);
        }
        straightResistance = avgvalue - 1;
        
        if(bentResistance > 0)
        {
          desiredResistance =  straightResistance - ((straightResistance - bentResistance) * rodMultiplier); 
          
        }
        #ifdef DEBUG
          Serial.print("CAL STRAIGHT: ");
          Serial.println(straightResistance);        
          Serial.print("DESIREDRESISTANCE: ");
          Serial.println(desiredResistance);       
        #endif
        Serial1.print("CALSTRAIGHT ");
        Serial1.println(straightResistance);
        delay(1000);
        Serial1.print("SETPOINT ");
        Serial1.println(desiredResistance);
        header = 0; //stop entering this if statement
      }

      if(header == 0xCB) //measure calibration bent
      { 
        avgvalue = 0;
        while(avgvalue == 0)
        {
          for(int i = 0; i < 50; i++)
          {
            myStats.add(analogRead(resistancePin));          
            delay(100);
          }
          avgvalue = myStats.average() + 1; //Make sure the code exits the while loop
        }
        bentResistance = avgvalue - 1; //Remove the extra 1 value
        if(straightResistance > 0)
        {
           desiredResistance =  straightResistance - ((straightResistance - bentResistance) * rodMultiplier); //Calculates our tension value we want. It will be 95% of the difference between our max and min values that were calibrated     
        }
        #ifdef DEBUG
          Serial.print("CAL BENT: ");
          Serial.println(bentResistance);      
          Serial.print("DESIREDRESISTANCE: ");
          Serial.println(desiredResistance);
        #endif
        Serial1.print("CALBENT ");
        Serial1.println(bentResistance);
        delay(1000);
        Serial1.print("SETPOINT ");
        Serial1.println(desiredResistance);
        header = 0; //stop entering this if statement
       }

       if(header == 0xCD)  // apply input calibration bent value
       {
        delay(1000);
        bentResistance = (double) doubleByte; // the received res value
        if(straightResistance > 0)
        {
           desiredResistance =  straightResistance - ((straightResistance - bentResistance) * rodMultiplier); //Calculates our tension value we want. It will be 95% of the difference between our max and min values that were calibrated     
        }
        #ifdef DEBUG
          Serial.print("CAL BENT: ");
          Serial.println(bentResistance);      
          Serial.print("DESIREDRESISTANCE: ");
          Serial.println(desiredResistance);
        #endif
        Serial1.print("CALBENT ");
        Serial1.println(bentResistance);
        delay(1000);
        Serial1.print("SETPOINT ");
        Serial1.println(desiredResistance);
        header = 0; //stop entering this if statement
       }

       if(header == 0xCE)  // apply input calibration straight value
       {
        delay(1000);
        straightResistance = (double) doubleByte; // the received res value
        if(bentResistance > 0)
        {
           desiredResistance =  straightResistance - ((straightResistance - bentResistance) * rodMultiplier); //Calculates our tension value we want. It will be 95% of the difference between our max and min values that were calibrated     
        }
        #ifdef DEBUG
          Serial.print("CAL STRAIGHT: ");
          Serial.println(straightResistance);      
          Serial.print("DESIREDRESISTANCE: ");
          Serial.println(desiredResistance);
        #endif
        Serial1.print("CALSTRAIGHT ");
        Serial1.println(straightResistance);
        delay(1000);
        Serial1.print("SETPOINT ");
        Serial1.println(desiredResistance);
        header = 0; //stop entering this if statement
       }

       if(header == 0xCF)  // apply input calibration desired value
       {
        delay(1000);
        desiredResistance = (double) doubleByte; // the received res value
        #ifdef DEBUG
          Serial.print("DESIREDRESISTANCE: ");
          Serial.println(desiredResistance);
        #endif
        Serial1.print("SETPOINT ");
        Serial1.println(desiredResistance);
        header = 0; //stop entering this if statement
       }

       if(header == 0xC0) //measure calibration desired
       { 
        avgvalue = 0;
        while(avgvalue == 0)
        {
          for(int i = 0; i < 50; i++)
          {
            myStats.add(analogRead(resistancePin));          
            delay(100);
          }
          avgvalue = myStats.average() + 1; //Make sure the code exits the while loop
        }
        desiredResistance = avgvalue - 1; //Remove the extra 1 value
        #ifdef DEBUG
          Serial.print("DESIREDRESISTANCE: ");
          Serial.println(desiredResistance);
        #endif
        Serial1.print("SETPOINT ");
        Serial1.println(desiredResistance);
        header = 0; //stop entering this if statement
       }
       
       if(header == 0xCC) //STOP:Halt
       { 
         changeSpeed(0, STOP);
         halt = true;
         depthReached = true;
         returned = true;
       }
         
       if(header == 0xAA) //STOP:Return at full speed
       {
         //changeSpeed(0, STOP);
         depthReached = true;
         halt = false;
         returned = false;
         if(atTop() == false) //Return the winch to its upright position and maintian (only if the winch isn't already up)
         {
           changeSpeed(FAST_IN_SPEED, UP);
         }
         if(atTop() == true && winchEncoder.read() <= REV(SLOW_DIST))
         {
           changeSpeed(0, STOP);
           winchEncoder.write(0);
           returned = true;
           header = 0; //stop entering this if statement
         }
       }
       
       if(header == 0xBB) //STOP:Return slower
       {
         //changeSpeed(0, STOP);
         depthReached = true;
         halt = false;
         returned = false;
         if(atTop() == false) //Return the winch to its upright position and maintian
           changeSpeed(SLOW_IN_SPEED, UP);
         if(atTop() == true && winchEncoder.read() <= REV(SLOW_DIST))
         {
           changeSpeed(0, STOP);
           winchEncoder.write(0);
           returned = true;
           header = 0; //stop entering this if statement
         }
       }
       
       if(header == 0xEE)//Stop the motor
         remoteStop(); 
       
       if(header == 0xDD)//Start the motor
         remoteStart();
       
       if(header == 0xDA)//Take a profile - normal operation
         takeProfile();
         
       if(returned == true && halt == false) //If all the other header values aren't true then go to this
       {
         state = MAINTAIN;
       }
       else
       {
         state = CHECK_BUFFER;
       }
       break;
     
    case MAINTAIN:
       warningCounter = 0;
       bottomHit = false;
       kickstart = true;
        //Serial.println("MAINTAIN");
       //if(atTop() == false || !digitalRead(downPin) == true)
       if(atTop() == false)   //if the winch isn't in the up position then
        { 
         changeSpeed(MAINTAIN_SPEED, UP);//change our speed to go up at the maintain speed
         //Serial.println("I'M SETTING THE SPEED TO BE UP AT MAINTAIN SPEED");
       }
       else{
         //Serial.println("I'VE SET THE SPEED TO BE 0 AND STOPPED");
         changeSpeed(0, STOP); //when we've reached the top then stop
         winchEncoder.write(0); //reset the winch encoder counter to 0
         //Serial.println("I'VE RESET THE WINCH COUNTER");
         returned = true;
       }
       state = CHECK_BUFFER;
       break;
   
} //end switch
  delay(2);
} //end loop

void changeSpeed(float newSpeed, uint8_t newDir)  {
  double ramp = RAMP_TIME;
  if(newDir == DOWN)
    ramp = RAMP_TIME_DOWN;

  //Serial.println(newSpeed);
  //If we want to go UP
  if(newDir == UP){
    newSpeed = 100 - newSpeed;
    //winch.currentDir = UP;
  }
  //If we want to go down
  else if(newDir == DOWN){
    newSpeed = 100 + newSpeed;
    //winch.currentDir = DOWN;
  }
  //Else we want to STOP
  else{
    newSpeed = 100;
    //winch.currentDir = STOP;
  }
  #ifdef DEBUG
    //Serial1.print("[Desired: ");
    //Serial1.print(newSpeed);
    //Serial1.print(" Current: ");
    //Serial1.print(winch.currentSpeed);
    //Serial1.println("]");
  #endif
  

  //Check if no change is needed (if we are going the desired speed and direction)
  if(newDir == winch.currentDir && newSpeed == winch.currentSpeed)
  {  //If the command is to continue moving the same speed and direction then don't do the sinusoid
    winch.prevSpeed = winch.currentSpeed;
    winch.prevDesiredSpeed = newSpeed; //Next time we write a new speed we know it will be at t0, the beginning of a speed change
    winch.prevDesiredDir = newDir;
    #ifdef DEBUG
      //Serial1.println("[REACHED END CASE]");
    #endif
    return;   //...return without altering speed (And ignoring EVERYTHING below this)
  }


    //This will catch if we have gotten to this spot while the previous call to the function was "no change requested"
    if(winch.prevDesiredSpeed != newSpeed && winch.prevDesiredDir != newDir)
    {  //Even though the newspeed = prevspeed will be true, this only activates when changing direction
      //If that's the case, then we want to initialize the acceleration
      t0 = millis();  //Start a timer for the sinusoid function
      speedDifference = newSpeed - winch.prevSpeed;
      //Avoid errors comparing different data types. If the difference is a negative value, make it slightly more negative. Same for a positive speed difference. 
      if(speedDifference < 0)
        speedDifference -= 1;
      else if (speedDifference > 0)
        speedDifference += 1;
    }   
    if(kickstart == true) 
    {
       uint64_t deltaT = millis() - t0;//Keep relative time for the sinusoid (this will not activate if we've reached the speed we want
      deltaT = constrain(deltaT, 0.0, ramp);
      #ifdef SINUSOID
      winch.currentSpeed = (double)winch.prevSpeed + (double)speedDifference*SINFACTOR*(1-cos((pi*(double)deltaT)/ramp));//Accelerate sinusoidally
      #endif
      #ifdef LINEAR 
      if(newDir == UP)
      {
         winch.currentSpeed = (newSpeed-100)*(deltaT/ramp) + 100; //will set newspeed to ramp up to the set speed in .5 secs (ramp millis)
      }
      else if(newDir == DOWN)
      {
        winch.currentSpeed = -newSpeed*(deltaT/ramp) + 100;
      }
      else
      {
        winch.currentSpeed = 100;
      }
      #endif
      
    }
    else
    {
      winch.currentSpeed = newSpeed;
      //Serial1.println("WRITING SPEED USING NON SINUIOD AS: ");
      //Serial.print(newSpeed);
    }


  constrain(winch.currentSpeed, 0, 200); //Do not write above or below the maximum pulse widths
   Serial.println("SPEED IS:   ");
   Serial.println(winch.currentSpeed);
  uint16_t speedToWrite = map(winch.currentSpeed, 0, 200, MAX_REVERSE, MAX_FORWARD); //Convert from sinusoid magnitude to pulse width
  uint16_t speedToTest = (winch.currentSpeed/200)*(MAX_FORWARD-MAX_REVERSE)+MAX_REVERSE;
  Serial.println("speedToWrite:   ");
  Serial.println(speedToWrite);
  Serial.println("speedToTest:   ");
  Serial.println(speedToTest);
  ESC.writeMicroseconds(speedToWrite); //Write the scaled value
  #ifdef DEBUG
    //Serial1.println(speedToWrite);
  #endif 
  if(winch.currentSpeed == newSpeed)
    winch.prevSpeed = newSpeed; //Set the starting point for the next speed change
    
  if(winch.currentSpeed > 100)
    winch.currentDir = DOWN;
  else if(winch.currentSpeed < 100)
    winch.currentDir = UP;
  else if(winch.currentSpeed == 100)
    winch.currentDir = STOP;
    
  winch.prevDesiredSpeed = newSpeed; //Next time we write a new speed we know it will be at t0, the beginning of a speed change
  winch.prevDesiredDir = newDir;
}

void serialEvent1()
{
  if(Serial1.available())
  {
   
   if (go) //check to see if the go statement is true (Data is synced)
   { 
     parameters[buffSize] = Serial1.read();
     buffSize++;
   }
     else //If the go statement isn't true, wait until the data is synced and then make go statement true
      {  
            firstbyte = Serial1.read(); //Read the first byte coming from the RPI
        
            if((firstbyte == 255) && (secondbyte == 255))
            {
              go = true;
            }
            else
            {
              secondbyte = firstbyte; //store value to check again
              go = false;
            }
     }
  }
}

void updateParameters(){
  header = parameters[0];
  speedOut = parameters[1]; //Save array contents to corresponding variables
  speedIn = parameters[2];
  upperByte = parameters[3];
  lowerByte = parameters[4];
  checksum = parameters[5];
  uByte = upperByte << 8;
  doubleByte = uByte + lowerByte;
  Serial.println(header);
  Serial.println(speedOut);
  Serial.println(speedIn);
  Serial.println(upperByte);
  Serial.println(lowerByte);
  Serial.println(checksum);
  buffSize = 0; //Reset buffer size and control variables
  kickstart = true; //reset kickstart for another profile
  go = false; //reset the go condition to check data again for consistency 
  secondbyte = 0x00; // reset second byte to zero
  warningCounter = 0;
  depthReached = false;
  bottomHit = false;
  //halt = false;
  if(checksum == ((((int)speedOut ^ (int)speedIn) ^ upperByte) ^ lowerByte))
  {
    depth = REV(doubleByte); //pings/revolution
    speedOut = speedOut*100/254; //Scale from 0-254 to 0 - 100
    speedIn  = speedIn*100/254; //Scale from 0-254 to 0 - 100
    dataCorrupted = false;
  }
  else
  {
    depth = 0;
    speedOut = 0;
    speedIn = 0;
    dataCorrupted = true;
  } 
}

void sendStatus(){
  Serial1.print("STATUS ");
  Serial.println("STATUS");
  if(dataCorrupted == false){
    if(returned == true){
      Serial1.print("1  "); //Ready
    }
    else{
      Serial1.print("0  "); //Busy
    }
  }
  else{
    Serial1.print("3  "); //Data corrupted
    dataCorrupted = false;
  }
  Serial1.print("Dir ");
  if(winch.currentDir == UP)
    Serial1.print("up  ");
  else if(winch.currentDir == DOWN)
    Serial1.print("down  ");
  else if(winch.currentDir == STOP)
    Serial1.print("stationary  ");
  Serial1.print("Rev ");
  long long pingsFromSurface = winchEncoder.read();
  pingsFromSurface = pingsFromSurface/3936;
  long revsFromSurface = (long) pingsFromSurface;
  Serial1.print(revsFromSurface);
  Serial1.print(" Res ");
  Serial1.print(analogRead(resistancePin));
  Serial1.print(" Spd ");
  Serial1.print(winch.currentSpeed);
  Serial1.print(" Ver ");
  Serial1.print(codeVersion);
  //Serial1.print(" AvSpd ");
  //Serial1.print(speedAVGold);
  //Serial1.print(" Msg ");
  //Serial1.print(bottomMsg);
  Serial1.print(speedAVGmsg);
  speedAVGmsg = "";
  //if(warningCounter > 150){
  //  Serial1.print(" HIGH RES");
  //}
  Serial1.println(" ");
  
  Serial.print("Rev ");
  Serial.println(revsFromSurface);
  Serial.print("Speed: ");
  Serial.println(winch.currentSpeed);
  Serial.print("Res: ");
  Serial.println(analogRead(resistancePin));
  Serial.print("Desired Resistance:  ");
  Serial.println(desiredResistance);
  Serial.print("PID ");
  Serial.println(speedTry);
  Serial.print("SPEED OUT ");
  Serial.print(speedOut);
  Serial1.print(" Ver ");
  Serial1.println(codeVersion);
  
  #ifdef DEBUG
    //Serial.print("[Resistance: ");
    //Serial.print(resistance);
    //Serial.println("]");
  #endif
}

inline void remoteStart(){
  if (motorRunning == false){ //Prevent remote start from executing if motor already running
    digitalWrite(remoteStopPin, LOW);
    digitalWrite(remoteStartPin, HIGH);
    digitalWrite(remoteStartLED, HIGH);
    delay(startTime);
    digitalWrite(remoteStartPin, LOW);
    digitalWrite(remoteStartLED, LOW);
    motorRunning = true;
  }
}

inline void remoteStop(){
  if(motorRunning == true){
    digitalWrite(remoteStopPin, HIGH);
    digitalWrite(remoteStopLED, HIGH);
    motorRunning = false;
  }
}

void takeProfile(){
   if((depthReached == false) && (halt == false)){
//    if(!digitalRead(downPin) == false){//Slowly let A-frame down from upright position
//      changeSpeed(30, DOWN);
//      returned = false;
//    }
     if((winchEncoder.read() < depth)){
     /* for(float i = 0; i < 40; i++)
      {
        changeSpeed(i,DOWN);
        delay(1500);
      }
      */
       doPID(speedOut, DOWN);
       returned = false;
     }
     else if(winchEncoder.read()>= depth){
       changeSpeed(0, STOP);
       Serial.println("I'VE REACHED THE END OF THE PROFILE, SET THE SPEED TO 0 AND STOP"); //debug change speed when reached depth desired
       depthReached = true; 
       returned = false;
       kickstart = true;
     }
  }
  else if((depthReached == true) && (halt == false)){
    
      speedAVG = 0.0;
      speedAVGold = 0.0;
      PIDCounter = 0;
      if(atTop() == true && winchEncoder.read() < REV(SLOW_DIST )) //Stop when A-frame is in full upright position
      { 
        changeSpeed(0, STOP);
        winchEncoder.write(0); //Account for line stretching - reset after each cast
        returned = true;
        Serial.println("DONE PROFILE");
        header = 0; //Will go straight into maintain mode rather than use take profile to maintain position
      }
     
      else if(winchEncoder.read() > REV(SLOW_DIST)){ //checks to see if we're within the slow_dist revolutions (5 revs)
      changeSpeed(speedIn, UP);
      //Serial.println("Going speedIn in the UP direction"); //Debug speed in
      returned = false;
    }
      else{ //We reached the depth and we're within 5 revs
        changeSpeed(LIFT_SPEED, UP); //This is the speed the winch will go when within five revolutions
      //Serial.println("Going Lift_Speed in the UP direction"); //debug speed in
      }
    }
  }



void doPID(int speedout, uint8_t newDir)
 {
  if(kickstart == true)
  {
    if(winch.currentSpeed < (speedout+100) && newDir == DOWN)
    {
      changeSpeed(speedout, newDir);
    }
    else
    {
      kickstart = false;
    }
  }
  else
  {
      if(newDir == DOWN) 
      {
          //Read the rod resistance (0 - 1023) and run the PID function
          
         /* for(int i = 0; i < 100; i++) //averages the increase value 
          {
            resistance = analogRead(resistancePin);
            rodPID.Compute();  // outputs a new speedTry
            myStats.add(speedTry);
            delay(1);
          } */
          resistance = analogRead(resistancePin);
          if(resistance > desiredResistance)
          {
            Kp = .0095;
            Ki = Kp/5;
            Kd = Kp/8;
          }
          else
          {
            Kp = .005;
            Ki = Kp/8;
            Kd = Kp/2.75;
          }
          delay(50);
          rodPID.SetTunings(Kp,Ki,Kd);
          rodPID.Compute();
          if(analogRead(resistancePin) <= desiredResistance)
          {
            warningCounter = 0; 
          }
          if(analogRead(resistancePin > straightResistance - (straightResistance - desiredResistance)*.3 ))
          {
            warningCounter = warningCounter + 1;
          }
          if(warningCounter > 150)
          {
            //header = 0xBB;  // go into "stop and return slow" mode in next iteration of the loop
          }
          //int mini = desiredResistance - bentResistance;
          //int maxi = desiredResistance - straightResistance;
          //map(speedTry, mini, maxi, -10, 10); // this should work in both up and down directions
          //castSpeedTry = (speedTry/(maxi-mini))*20-10;
           //if(resistance > desiredResistance)
          //{
            //delay(10);
          //}
          speedOut = speedTry + speedOut;
          speedOut = constrain(speedOut, STOP_SPEED, 40);
          //delay(50);
          changeSpeed(speedOut, newDir); //writes the new speed to the changeSpeed function
          
          PIDCounter = PIDCounter + 1;
          if(PIDCounter < 11)
          {
            speedAVG = speedAVG + speedOut;
          }          
          else
          {
            speedAVG = speedAVG / 10;
            if(speedAVG < speedAVGold - SPEED_DIFF)
            {
              bottomHit = true;
              header = 0xBB;  // go into "stop and return slow" mode in next iteration of the loop
              bottomMsg = "BOTTOM";
            }
            else {
              bottomMsg = "OK";
            }
            speedAVGold = speedAVG;
            speedAVGmsg = speedAVGmsg + " AvSpd " + String(speedAVG) + " Msg " + bottomMsg;
            speedAVG = 0.0;
            PIDCounter = 0;          
          }
       
       
       
       /*   #ifdef DEBUG
            Serial.print("[Resistance: ");
            Serial.print(resistance);
            Serial.println("PID Output:   ");
            Serial.print(speedTry);
            Serial.println("SPEED IS: ");
            Serial.println(speedOut);
            //Serial.println("]");
            
          #endif */
      }
  }
  }

bool atTop(){
  bool pinvalue = false;
  // this logic is for the push switch (which doesn't require reversal)
  //if (digitalRead(upPin) == true) { 
  //  pinvalue = true;
  //} else {
  //  pinvalue = false;
  //}
  // this logic is for the Hall Effect sensor (which requires reversal)
  if (digitalRead(upPin) == true) { 
    pinvalue = false;
  } else {
    pinvalue = true;
  }
  return pinvalue;
}

