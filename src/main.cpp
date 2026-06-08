#include <iostream>
#include <string>
#include <tree_sitter/api.h> //core tree-sitter C api
#include <memory>
#include <sstream>
#include <fstream>

using namespace std;
//declare c++ parser function that we are linking from our submodule.
//extern 'C' tells the C++ compiler not to mangle the C function name.
extern "C" TSLanguage *tree_sitter_cpp();

//read file from hard drive
string read_file(const string& filepath){
    ifstream file(filepath);
    if(!file.is_open()){
        cerr<<"Error: Could not open file "<<filepath<<endl;
        exit(1);
    }
    stringstream buffer;
    buffer<<file.rdbuf();
    return buffer.str();

}

struct ParserDeleter{
    void operator()(TSParser* p)const{
        if(p) ts_parser_delete(p);
    }
};

struct TreeDeleter{
    void operator()(TSTree* t)const{
        if(t) ts_tree_delete(t);
    }
};

struct QueryDeleter{
    void operator()(TSQuery* q)const{
        if(q) ts_query_delete(q);
    }
};

struct QueryCursorDeleter{
    void operator()(TSQueryCursor* qc)const{
        if(qc) ts_query_cursor_delete(qc);
    }
};

using ParserPtr = unique_ptr<TSParser, ParserDeleter>;
using TreePtr = unique_ptr<TSTree, TreeDeleter>;
using QueryPtr = unique_ptr<TSQuery, QueryDeleter>;

using CStringPtr = unique_ptr<char,decltype(&free)>;
using QueryCursorPtr= unique_ptr<TSQueryCursor,QueryCursorDeleter>;

int main(){
    cout<<"Starting fastgraphrag AST Parser..."<<endl;

    //1. create new parser instance
    ParserPtr parser(ts_parser_new());

    //2. tell the parser to use c++ grammar rules
    ts_parser_set_language(parser.get(), tree_sitter_cpp());

    //3. dummy code
    string src_code="int add(int a,int b){return a+b;}\n"
                    "void print_hello(){cout<<\"Hello\";}";
    cout<<"Analyzing:\n"<<src_code<<'\n'<<endl;

    //4. parse the string into an AST(Abstract Syntax Tree)
    TreePtr tree(
        ts_parser_parse_string(
            parser.get(),
            nullptr,
            src_code.c_str(),
            src_code.length()
        )
    );

    // 5. get the root node of the tree(top of hierarchy)
    TSNode root_node = ts_tree_root_node(tree.get());

    //=====
    //Extracting data using queries
    //=====

    // 1. query to find a function definition:
    // look inside its declarator, find the identifier (the name),
    // and tag it as @func_name

    string query_str= "(function_definition declarator: (function_declarator declarator:(identifier) @func_name))";

    uint32_t error_offset;
    TSQueryError error_type;

    // 2. compile query
    QueryPtr query(
        ts_query_new(
            tree_sitter_cpp(),
            query_str.c_str(),
            query_str.length(),
            &error_offset,
            &error_type
        )
    );
    
    if(error_type != TSQueryErrorNone){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }
    //error in returned pointer
    if(!query){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }

    // 3. cursor to execute the query against our syntax tree
    QueryCursorPtr query_cursor(
        ts_query_cursor_new()
    );
    ts_query_cursor_exec(
        query_cursor.get(),
        query.get(),
        root_node
    );

    TSQueryMatch match;
    cout<<"=== Extracted Functions ==="<<endl;

    // 4. loop through all the matches found in code
    while(ts_query_cursor_next_match(query_cursor.get(), &match)){
        for(uint32_t i=0;i<match.capture_count;i++){
            TSNode capture_node = match.captures[i].node;

            //start and end byte positions of func_name
            uint32_t start_byte=ts_node_start_byte(capture_node);
            uint32_t end_byte=ts_node_end_byte(capture_node);

            string func_name=src_code.substr(start_byte,end_byte-start_byte);

            cout<<"Found Function: -> "<<func_name<< " <-"<<endl;
        }
    }

    // //6. convert the tree to readable string format(S-expression)
    // CStringPtr tree_string(
    //     ts_node_string(root_node),
    //     &free
    // );

    // cout<<"=== Abstract Syntax Tree ==="<<endl;
    // cout<<"Analyzing Code: int add(int a,int b){return a+b;}\n"<<tree_string.get()<<endl;

    return 0;
}