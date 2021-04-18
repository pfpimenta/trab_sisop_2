
#include "../include/MasterTable.hpp"

// mutexes
pthread_mutex_t read_write_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t reader_mutex = PTHREAD_MUTEX_INITIALIZER;
int reader_counter = 0;

void shared_reader_lock(){
	pthread_mutex_lock(&reader_mutex);
	reader_counter++;
	if(reader_counter == 1){
		pthread_mutex_lock(&read_write_mutex);
	}
	pthread_mutex_unlock(&reader_mutex);
}

void shared_reader_unlock(){
	pthread_mutex_lock(&reader_mutex);
	reader_counter--;
	if(reader_counter == 0){
		pthread_mutex_unlock(&read_write_mutex);
	}
	pthread_mutex_unlock(&reader_mutex);
}


// saves the master_table into a TXT file
void MasterTable::save_backup_table()
{
	//master_table
	std::list<std::string> followers;
	std::string username;
	Row* row;
	std::ofstream table_file;
	table_file.open ("backup_table.txt", std::ios::out | std::ios::trunc); 
	for(auto const& x : this->table)
	{	
		username = x.first;
		row = x.second;
		followers = row->getFollowers();

		table_file << username;
		table_file << ".";
		
		for (std::string follower : followers)
		{
			table_file << follower;
			table_file << ",";
		}
		table_file << "\n";
	}
	table_file.close(); 
}

// inline bool file_exists (const std::string& name) {
//     return ( access( name.c_str(), F_OK ) != -1 );
// }

// loads the master_table from a TXT file, if it exists
void MasterTable::load_backup_table()
{
	char* line_ptr_aux;
	char* token;
	Row* row;

	// if file exists
    const std::string& filename = "backup_table.txt";
	if(access( filename.c_str(), F_OK ) != -1 )
	{
		printf("Restoring backup... \n");
		fflush(stdout);
		std::ifstream table_file("backup_table.txt");
		for( std::string line; getline( table_file, line ); )
		{
			char* line_ptr = strdup(line.c_str());

			row = new Row;
			strcpy(line_ptr, line.c_str());

			token = strtok_r(line_ptr, ".", &line_ptr_aux);
			std::string username(token);

			token = strtok_r(NULL, ",", &line_ptr_aux);
			while(token != NULL)
			{
				std::string follower(token);
				row->setAddNewFollower(follower);
				token = strtok_r(NULL, ",", &line_ptr_aux);
			}

			// insert new (usename, row) in master_table
			pthread_mutex_lock(&read_write_mutex);
			this->table.insert( std::make_pair(username, row) );
			pthread_mutex_unlock(&read_write_mutex);
		}
		table_file.close(); 
	} else {
		printf("Backup table not found. Creating new. \n");
		fflush(stdout);
	}
}



void MasterTable::sendMessageToFollowers(std::string username, std::string message)
{
    // TODO consertar mutexes
    shared_reader_lock();
    Row* currentRow = this->table.find(username)->second;
    shared_reader_unlock();
    std::list<std::string> followers = currentRow->getFollowers();
    for (std::string follower : followers){
        shared_reader_lock();
        Row* followerRow = this->table.find(follower)->second;
        shared_reader_unlock();
        followerRow->addNotification(username, message);
    }
}

void MasterTable::deleteRows()
{
    for (auto const& x : this->table) {
        delete(x.second);
    }
}