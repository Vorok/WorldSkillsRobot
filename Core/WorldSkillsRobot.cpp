#include <iostream>
#include <unistd.h>
#include <signal.h>
#include "mraa.hpp"
#include <curl/curl.h>
#include "json.hpp"
#include <vector>
//json.hpp взят из https://github.com/nlohmann/json
//для компиляции просто перетащить json.hpp в папку с проектом
#include <memory>
#include <string>
//компиляция: g++ -std=c++11 robot_wsg.cpp -lmraa -lcurl -o a.out

// Моторы подключаются к клеммам M1+, M1-, M2+, M2-
// Если полюса моторов окажутся перепутаны при подключении,
// можно изменить соответствующие константы CON_MOTOR с 0 на 1
#define CON_MOTOR1 1
#define CON_MOTOR2 0

// Motor shield использует четыре контакта 4, 5, 6, 7 для управления моторами
// 4 и 7 — для направления, 5 и 6 — для скорости
#define SPEED_1 5
#define DIR_1 4

#define SPEED_2 6
#define DIR_2 7

//Сервоприводы двигаются с разной скоростью. Коэф это исправляет
#define COEFF_RIGHT 0.87
#define COEFF_LEFT 1

// Возможные направления движения робота
#define FORWARD 1
#define BACKWARD 2
#define LEFT 3
#define RIGHT 4
#define NOTHING 0

#define DEFAULT_URL "https://academic-educatorsextension.portal.ptc.io/Thingworx/Things/manage_robot_rost5000/Services/questMe?appKey=2ab91b18-2bd2-4ddf-b4e5-e7e25749274e&method=post&x-thingworx-session=true"


using namespace mraa;
using json = nlohmann::json;

std::vector<std::string> cmd = {"nothing", "forward", "backward", "left", "right"};

//прерывание по Ctrl+c
int running = 1;
void sig_handler(int signo)
{
    if (signo == SIGINT) {
        printf("closing down\n");
        running = 0;
    }
}

//класс отвечает за общение с сервером.
class RobotNetwork
{
private:
	//коллбэк для curl
	static std::size_t callback(
            const char* in,
            std::size_t size,
            std::size_t num,
            std::string* out)
    {
        const std::size_t totalBytes(size * num);
        out->append(in, totalBytes);
        return totalBytes;
    }
	
public:
	//костыль 1: забрать json с html страницы ThingWorx
	static std::string parseJsonFromHtml(std::string str)
    {
        std::string res = "";
        bool flagJSON = false;
        for(auto a : str)
        {
            if(a == '{')
            {
                flagJSON = true;
            }
            if(a == '}')
            {
                res+=a;
                break;
            }
            if(flagJSON)
            {
                res+=a;
            }
        }
        return res;
    }
	
	//костыль 2: проблема с кодировской спец символов
	static void fixString(std::string &str)
	{
		size_t f = -1;
		while((f = str.find("&quot;")) != std::string::npos)
		{
			str.replace(f, 6, "\"");
			//std::cout << str << std::endl << std::endl;
		}
		while((f = str.find("&#x7b;")) != std::string::npos)
		{
			str.replace(f, 6, "{");
		}
		while((f = str.find("&#x7d;")) != std::string::npos)
		{
			str.replace(f, 6, "}");
		}
		while((f = str.find("&#x3a;")) != std::string::npos)
		{
			str.replace(f, 6, ":");
		}
	}
	
	//коннект к ThingWorx
	int connect(std::string params)
	{
		const std::string url(DEFAULT_URL + params);

		CURL* curl = curl_easy_init();

		// Set remote URL.
		curl_easy_setopt(curl, CURLOPT_URL, url.c_str());

		// Don't bother trying IPv6, which would increase DNS resolution time.
		curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);

		// Don't wait forever, time out after 10 seconds.
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);

		// Follow HTTP redirects if necessary.
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

		// Response information.
		int httpCode(0);
		std::unique_ptr<std::string> httpData(new std::string());

		// Hook up data handling function.
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);

		// Hook up data container (will be passed as the last parameter to the
		// callback handling function).  Can be any pointer type, since it will
		// internally be passed as a void pointer.
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, httpData.get());

		// Run our HTTP GET command, capture the HTTP response code, and clean up.
		curl_easy_perform(curl);
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
		curl_easy_cleanup(curl);

		//если соединение успешно:
		if (httpCode == 200)
		{
			std::cout << "\nGot successful response from " << url << std::endl;
			//std::cout << *httpData << std::endl << std::endl;
			fixString(*httpData);
			//std::cout << *httpData << std::endl;
			std::string jsonData = parseJsonFromHtml(*httpData);
			std::cout << jsonData << std::endl;
			json j = json::parse(jsonData);
			std::string command = j.value("comand", "nothing");
			std::cout << command << std::endl;
			//возвращает команду по номеру. TODO переделать вектор в enum?
			for(int i = 0; i < cmd.size(); ++i)
			{
				if (command.compare(cmd[i]) == 0)
					return i;
			}
			/*if (command.compare("nothing") == 0)
				return NOTHING;
			if (command.compare("forward") == 0)
				return FORWARD;
			if (command.compare("backward") == 0)
				return BACKWARD;
			if (command.compare("left") == 0)
				return LEFT;
			if (command.compare("right") == 0)
				return RIGHT;*/
			return NOTHING;
		}
		else
		{
			std::cout << "Couldn't GET from " << url << " - exiting" << std::endl;
			return -2;
		}

		return -2;
	}

};

//класс отвечает за движение
class RobotDriver
{
private:
    Gpio dir1;
    Gpio dir2;
    Pwm spd1;
    Pwm spd2;

public:
    RobotDriver() : dir1(DIR_1), dir2(DIR_2), spd1(SPEED_1), spd2(SPEED_2)
    {
        dir1.dir(DIR_OUT);
        dir2.dir(DIR_OUT);
    }
	//если подать на spd1,2 0, то колеса застревают в движении. особеность edison.
	void stop()
	{
		usleep(100000);
		//spd1.write(0.05);
		//spd2.write(0.05);
		spd1.write(0.0005);
		spd2.write(0.0005);
		//spd1.write(0.0);
		//spd2.write(0.0);
		dir1.write(CON_MOTOR1);
		dir2.write(CON_MOTOR2);
		usleep(500000);
	}
	
	//движение в определенную сторону с указанной скоростью.
    void go(int newDirection, float speed)
    {
      bool motorDirection_1, motorDirection_2;
	  float speedL = speed;
	  float speedR = speed;

      switch ( newDirection ) {

        case FORWARD:
            motorDirection_1 = true;
            motorDirection_2 = true;
			speedR *= COEFF_RIGHT;
            break;
        case BACKWARD:
            motorDirection_1 = false;
            motorDirection_2 = false;
			speedR *= COEFF_RIGHT;
            break;
        case LEFT:
            motorDirection_1 = true;
            motorDirection_2 = false;
            break;
        case RIGHT:
            motorDirection_1 = false;
            motorDirection_2 = true;
            break;
      }

      // Если мы ошиблись с подключением - меняем направление на обратное
      motorDirection_1 = CON_MOTOR1 ^ motorDirection_1;
      motorDirection_2 = CON_MOTOR2 ^ motorDirection_2;

      // Скорость может меняться в пределах от 0 до 1.
	  if(speed > 0.0)
	  {
		  spd1.write(speedR);
		  spd2.write(speedL);
		  spd1.enable(true);
		  spd2.enable(true);
	  }else{
		  spd1.write(0);
		  spd2.write(0);
	  }


      dir1.write(motorDirection_1);
      dir2.write(motorDirection_2);
    }
	
	//один шаг (или поворот) в указанном направлении
	void step(int direction)
	{
		switch ( direction ) {
        case FORWARD:
            go(FORWARD, 0.95);
			std::cout << "cmd move forward" << std::endl;
			usleep(290000);
            break;
        case BACKWARD:
            go(BACKWARD, 0.75);
			std::cout << "cmd move backward" << std::endl;
			usleep(220000);
            break;
        case LEFT:
            go(LEFT, 0.3);
			std::cout << "cmd turn left" << std::endl;
			usleep(300000);
            break;
        case RIGHT:
            go(RIGHT, 0.3);
			std::cout << "cmd move right" << std::endl;
			usleep(300000);
            break;
		case NOTHING:
			usleep(1000000);
			break;
		default:
			usleep(1000000);
			break;
		}

		stop();
	}
};



int main()
{
	signal(SIGINT, sig_handler);
    RobotDriver robotDriver;
    RobotNetwork robotNetwork;
	
    // Задержка 3 секунд после включения питания
	usleep(3000000);
	std::cout << "start!" << std::endl;
	
	int lastCommand = NOTHING;

	//цикл прерывается по ctrl+c
	while(running)
	{
		//список посылаемых параметров
		std::string params("&state=\"ready!_last_cmd=" + cmd[lastCommand]+ "\"");
		
		//в answer - FORWARD, LEFT, RIGHT, BACKWARD или NOTHING; также -2 при ошибки подкл.
		int answer = robotNetwork.connect(params);
		robotDriver.step(answer);
		if (answer != NOTHING)
			lastCommand = answer;
	}
	
	//Дебаг:
    // Медленный разгон до максимальной скорсти
	/*std::cout << "accel!" << std::endl;
    for (int i=50; i<=1000; i+=50)
    {
      robotDriver.go(FORWARD, ((float)i)/1000);
      usleep(100000);
    }
	usleep(2000000);
	std::cout << "deccel!" << std::endl;
	for (int i=1000; i>=50; i-=50)
    {
      robotDriver.go(FORWARD, ((float)i)/1000);
      usleep(100000);
    }

    usleep(2000000);*/

	/*while(running)
	{
		robotDriver.go(FORWARD, 0.75);
		std::cout << "forward!" << std::endl;
		usleep(1000000);
		//robotDriver.go(BACKWARD, 0.75);
		//std::cout << "backward!" << std::endl;
		//usleep(3000000);
		robotDriver.stop();
		std::cout << "stop!" << std::endl;
		usleep(3000000);
	}*/
	/*robotDriver.step(FORWARD);
	robotDriver.step(FORWARD);
	robotDriver.step(FORWARD);
	robotDriver.step(FORWARD);
	robotDriver.step(FORWARD);
	robotDriver.step(FORWARD);
	robotDriver.step(FORWARD);
	robotDriver.step(LEFT);
	robotDriver.step(FORWARD);
	robotDriver.step(RIGHT);
	robotDriver.step(FORWARD);
	robotDriver.step(FORWARD);
	robotDriver.step(BACKWARD);*/
	
	std::cout << "stop! program over." << std::endl;
	robotDriver.stop();
}
