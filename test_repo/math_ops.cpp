#include <iostream>

using namespace std;

//simple multiply function
int multiply(int x, int y){
    return x*y;
}

//more complex function that calls multiply
void calculate_and_print(int a, int b){
    int result=multiply(a,b);
    cout<<"The result is: "<<result<<'\n';
}