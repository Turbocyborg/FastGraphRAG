#include <iostream>
#include <string>
#include <tree_sitter/api.h> //core tree-sitter C api
#include <memory>
#include <sstream>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <set>
#include <hnswlib/hnswlib.h> //include vectorDB

using namespace std;
//declare c++ parser function that we are linking from our submodule.
//extern 'C' tells the C++ compiler not to mangle the C function name.
extern "C" TSLanguage *tree_sitter_cpp();

//======
//Graph Data Structure
//======
struct FunctionNode {
    size_t id; //unique ID for vectorDB
    std::string name;
    std::string body;
    set<std::string>calls_to;//edges! which functions does this one call
};

// the actual knowledge graph(adjacency list)
// maps function name to its node data
unordered_map<std::string,FunctionNode>knowledge_graph;
//to look up a function by its vector DB ID
unordered_map<size_t,string>id_to_name;

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

//=====
// MOCK AI EMBEDDING FUNCTION
//=====
// in a real app, an LLM gives us a vector of 1536 floats
// for now,lets generate a fake 16-dimensional vector based on the string length and char
vector<float>mock_embed(const string& text){
    vector<float>vec(16,0.0f);
    for(size_t i=0;i<text.length();i++){
        vec[i%16]+=(float)text[i]*0.1f;
    }
    return vec;
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
using QueryCursorPtr= unique_ptr<TSQueryCursor,QueryCursorDeleter>;

using CStringPtr = unique_ptr<char,decltype(&free)>;


int main(){
    cout<<"Starting fastgraphrag Engine..."<<endl;

    //1. create new parser instance
    ParserPtr parser(ts_parser_new());

    //2. tell the parser to use c++ grammar rules
    ts_parser_set_language(parser.get(), tree_sitter_cpp());

    //3. actual file from test repo
    string filepath="../test_repo/math_ops.cpp";
    string src_code=read_file(filepath);
    cout<<"Successfully loaded: "<< filepath << " ("<< src_code.length()<<"bytes)\n"<<endl;



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

    //OUTER QUERY:

    // 1. query to capture both name(@func.name) and entire definition(@func.body):
    // look inside its declarator

    string query_str= "((function_definition declarator: (function_declarator declarator:(identifier) @func.name)) @func.body)";

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

    //INNER QUERY:
    // find the function calls inside function body
    string call_query_str="(call_expression function: (identifier) @called)";
    //compile query
    QueryPtr call_query(
        ts_query_new(
            tree_sitter_cpp(),
            call_query_str.c_str(),
            call_query_str.length(),
            &error_offset,
            &error_type
        )
    );

    if(error_type != TSQueryErrorNone){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }
    //error in returned pointer
    if(!call_query){
        cerr<<"Query Error at offset: "<<error_offset<<endl;
        return 1;
    }

    // 3. cursor to execute the query against our syntax tree
    QueryCursorPtr query_cursor(
        ts_query_cursor_new()
    );
    //cursor for inner search
    QueryCursorPtr call_cursor(
        ts_query_cursor_new()
    );

    ts_query_cursor_exec(
        query_cursor.get(),
        query.get(),
        root_node
    );

    TSQueryMatch match;
    //unique ID counter
    size_t curr_id=0;

    cout<<"=== RAG Chunks Extracted ===\n"<<endl;

    // 4. loop through all the matches found in code
    while(ts_query_cursor_next_match(query_cursor.get(), &match)){
        // string curr_func_name="";
        // string curr_func_body="";
        FunctionNode node;
        //assign ID and increment
        node.id=curr_id++;
        TSNode body_node;

        //loop through the captures in this match
        for(uint32_t i=0;i<match.capture_count;i++){
            TSNode capture_node = match.captures[i].node;

            //start and end byte positions of func_name
            uint32_t start_byte=ts_node_start_byte(capture_node);
            uint32_t end_byte=ts_node_end_byte(capture_node);

            //extracted text
            string extracted_txt=src_code.substr(start_byte,end_byte-start_byte);

            //get the name of capture tag(e.g., "func.name" or "func.body")
            uint32_t length;
            const char *capture_name = ts_query_capture_name_for_id(query.get(), match.captures[i].index, &length);

            string tag(capture_name, length);

            if(tag=="func.name")node.name=extracted_txt;
            if(tag=="func.body"){
                node.body=extracted_txt;
                body_node=capture_node; // save the AST node of body so we can search inside it
            }

        }
        //=====
        // Graph Building: Find outgoing edges
        //=====
        //Execute the inner query Only inside the current function's body
        ts_query_cursor_exec(
            call_cursor.get(),
            call_query.get(),
            body_node
        );
        TSQueryMatch call_match;

        while(ts_query_cursor_next_match(call_cursor.get(), & call_match)){
            TSNode call_node= call_match.captures[0].node;
            uint32_t start = ts_node_start_byte(call_node);
            uint32_t end = ts_node_end_byte(call_node);
            string called_func_name= src_code.substr(start,end-start);

            //add edge to our node
            node.calls_to.insert(called_func_name);
        }

        //add the completed node to our knowledge graph
        knowledge_graph[node.name]=node;
        //save the mapping!
        id_to_name[node.id]=node.name;

        // //print perfect "chunk" ready for a vector database
        // cout<<"[Function Name]: "<<curr_func_name<<endl;
        // cout<<"[Code Chunk]:\n"<<curr_func_body<<'\n'<<endl;
        // cout<<"-----------------------------------"<<endl;
    }

    //=====
    //VECTOR DATABASE(hnswlib)
    cout<<"Building Vector Index...\n";

    int dim=16;//size of our vectors
    int mx_elements=10000;//max capacity of our database

    //1. init the Math space(inner product is great for cosine similarity)
    hnswlib::InnerProductSpace space(dim);
    //2. init the HNSW index(the database)
    //M=16(number of connections/element) ef_construction=200(search depth during build)
    hnswlib::HierarchicalNSW<float>* vector_db=new hnswlib::HierarchicalNSW<float>(&space, mx_elements,16,200);

    // 3.insert our functions into database
    for(const auto&pair:knowledge_graph){
        const FunctionNode& node =pair.second;

        //generate Embedding
        //generate the vector from function's code
        vector<float> embedding = mock_embed(node.body);

        //add it to the database(requires pointer to vector array, and id)
        vector_db->addPoint(embedding.data(),node.id);
        cout<<"Inserted ["<<node.name<<"] into Vector DB with ID: "<<node.id<<'\n';

    }

    //=====
    //Searching the Database
    //=====
    cout<<"\n=== AI Semantic Search Test ===\n";
    string user_ques="How do I calculate and print something?";
    cout<<"User Question: "<<user_ques<<'\n';

    //1.convert the user's question into a vector
    vector<float>query_vector=mock_embed(user_ques);

    //2.search for the Top 1 closest match(k=1)
    auto res=vector_db->searchKnn(query_vector.data(),1);

    if(!res.empty()){
        size_t best_match_id=res.top().second;
        string best_match_name=id_to_name[best_match_id];

        cout<<"-> Nearest Code Chunk Found: "<<best_match_name<<'\n';

        //lets use our graph to get extra content
        cout<<"-> Graph Context: This function also calls: ";
        for(const auto& edge:knowledge_graph[best_match_name].calls_to){
            cout<<edge<<" ";
        }
        cout<<'\n';
    }

    //clean up memory
    delete vector_db;

    // //=====
    // //Print the knowledge graph
    // //=====
    // cout<<"=== In-Memory Knowledge Graph ===\n";
    // for(const auto&pair : knowledge_graph){
    //     cout<<"Node: ["<<pair.first<<"]\n";

    //     if(pair.second.calls_to.empty()){
    //         cout<<" Edges: None (Leaf node)\n";
    //     }else{
    //         for(const auto& edge: pair.second.calls_to){
    //             cout<<" --[CALLS]--> Node: ["<<edge<<"]\n";
    //         }
    //     }
    //     cout<<"------------------------------\n";
    // }

    // //6. convert the tree to readable string format(S-expression)
    // CStringPtr tree_string(
    //     ts_node_string(root_node),
    //     &free
    // );

    // cout<<"=== Abstract Syntax Tree ==="<<endl;
    // cout<<"Analyzing Code: int add(int a,int b){return a+b;}\n"<<tree_string.get()<<endl;

    return 0;
}