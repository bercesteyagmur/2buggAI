#include <pthread.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <stdio.h>
#include <stdlib.h>

// These variables need to be global, since they are used by the threads

// semaphores ... 
int barrier1, barrier2;
int turn1;

void
create_semaphore1 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  // set up semaphore 1
  if ((turn1 = semget (IPC_PRIVATE, 1, 0666)) != -1)
    {
      arg.val = 1;
      if (semctl (turn1, 0, SETVAL, arg) == -1)
	{
	  perror ("turn1 -- initialization ");
	  // delete sem
	  arg.val = 0;
	  semctl (turn1, -1, IPC_RMID, arg);
	  exit (EXIT_FAILURE);
	}			// end semctl failed
    }
  else
    {
      perror ("turn1 -- creation ");
      exit (EXIT_FAILURE);
    }				// end semget failed
  arg.val = 0;
  fprintf (stdout, "Inititialized: Semaphore turn1 %d\n",
	   semctl (turn1, 0, GETVAL, arg));
}

void
destroy_semaphore1 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  arg.val = 0;
  semctl (turn1, -1, IPC_RMID, arg);
  fprintf (stdout, "Semaphore turn 1 destroyed\n");
}

void
enter_critical_region (int child_nr)
{

  // semaphore operation to wait
  struct sembuf acquire = { 0, -1, SEM_UNDO };

  if (semop (turn1, &acquire, 1) == -1)
    {
      perror ("turn1 -- acquire ");
      exit (EXIT_FAILURE);
    }				// end acquire failure


  // we're inside now !
  fprintf (stdout, "%d is entering \n", child_nr);
}

void
leave_critical_region (int child_nr)
{
  // semaphore operation to signal
  struct sembuf release = { 0, 1, SEM_UNDO };

  if (semop (turn1, &release, 1) == -1)
    {
      perror ("turn1 -- release ");
      exit (EXIT_FAILURE);
    }				// end release failure
  fprintf (stdout, "%d left \n", child_nr);
}

void
create_barrier1 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  // set up semaphores
  if ((barrier1 = semget (IPC_PRIVATE, 1, 0666)) != -1)
    {
      arg.val = 0;
      if (semctl (barrier1, 0, SETVAL, arg) == -1)
	{
	  perror ("barrier1 -- initialization ");
	  // delete sem
	  arg.val = 0;
	  semctl (barrier1, -1, IPC_RMID, arg);
	  exit (EXIT_FAILURE);
	}			// end semctl failed
    }
  else
    {
      perror ("barrier1 -- creation ");
      exit (EXIT_FAILURE);
    }				// end semget failed

  arg.val = 0;
  fprintf (stdout, "created: Barrier1 %d\n",
	   semctl (barrier1, 0, GETVAL, arg));
}

void
activate_barrier1 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  arg.val = 0;
  if (semctl (barrier1, 0, SETVAL, arg) == -1)
    {
      perror ("barrier1 -- activation ");
      // delete sem
      arg.val = 0;
      semctl (barrier1, -1, IPC_RMID, arg);
      exit (EXIT_FAILURE);
    }				// end semctl failed
  // the teacher rules again
  arg.val = 0;
  fprintf (stdout, "activated: Barrier1 %d\n",
	   semctl (barrier1, 0, GETVAL, arg));
}

void
release_barrier1 (int child_count)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  arg.val = child_count;
  if (semctl (barrier1, 0, SETVAL, arg) == -1)
    {
      perror ("barrier1 -- release");
      // delete sem
      arg.val = 0;
      semctl (barrier1, -1, IPC_RMID, arg);
      exit (EXIT_FAILURE);
    }				// end semctl failed
  arg.val = 0;
  fprintf (stdout, "released %d children at barrier1 %d\n",
	   child_count, semctl (barrier1, 0, GETVAL, arg));
  // we're in the game now !
}

void
destroy_barrier1 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  arg.val = 0;
  semctl (barrier1, -1, IPC_RMID, arg);
  fprintf (stdout, "Barrier1 destroyed\n");
}

void
create_barrier2 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  // set up semaphores
  if ((barrier2 = semget (IPC_PRIVATE, 1, 0666)) != -1)
    {
      arg.val = 0;
      if (semctl (barrier2, 0, SETVAL, arg) == -1)
	{
	  perror ("barrier2 -- initialization ");
	  // delete sem
	  semctl (barrier2, -1, IPC_RMID);
	  exit (EXIT_FAILURE);
	}			// end semctl failed
    }
  else
    {
      perror ("barrier2 -- creation ");
      exit (EXIT_FAILURE);
    }				// end semget failed

  arg.val = 0;
  fprintf (stdout, "created: Barrier2 %d\n",
	   semctl (barrier2, 0, GETVAL, arg));
}

void
activate_barrier2 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  arg.val = 0;
  if (semctl (barrier2, 0, SETVAL, arg) == -1)
    {
      perror ("barrier2 -- activation ");
      // delete sem
      semctl (barrier2, -1, IPC_RMID);
      exit (EXIT_FAILURE);
    }				// end semctl failed
  // the teacher rules again
  arg.val = 0;
  fprintf (stdout, "activated: Barrier2 %d\n",
	   semctl (barrier2, 0, GETVAL, arg));
}

void
destroy_barrier2 (void)
{
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  arg.val = 0;
  semctl (barrier2, -1, IPC_RMID, arg);
  fprintf (stdout, "Barrier2 destroyed\n");
}


void *
child (void *child_nr)		// Called as thread
{
  struct sembuf acquire1 = { 0, -1, SEM_UNDO };
  struct sembuf release2 = { 0, 1, SEM_UNDO };
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  for (;;)
    {
      // semaphore operation to wait before barrier 1
      arg.val = 0;
      fprintf (stdout, "child %d before barrier1 acquire: %d\n",
	       (int) child_nr, semctl (barrier1, 0, GETVAL, arg));

      if (semop (barrier1, &acquire1, 1) == -1)
	{
	  perror ("child: barrier2-- acquire ");
	  exit (EXIT_FAILURE);
	}			// end acquire failure

      arg.val = 0;
      fprintf (stdout, "child %d after barrier1 acquire: %d\n",
	       (int) child_nr, semctl (barrier1, 0, GETVAL, arg));

      enter_critical_region ((int) child_nr);

      leave_critical_region ((int) child_nr);

      // semaphore operation to wait before barrier 2

      arg.val = 0;
      fprintf (stdout, "child %d before barrier2 release: %d\n",
	       (int) child_nr, semctl (barrier2, 0, GETVAL, arg));

      if (semop (barrier2, &release2, 1) == -1)
	{
	  perror ("child: barrier2 -- release ");
	  exit (EXIT_FAILURE);
	}			// end acquire failure
      arg.val = 0;
      fprintf (stdout,
	       "child %d after barrier2 release attempt, barrier2: %d\n",
	       (int) child_nr, semctl (barrier2, 0, GETVAL, arg));
    }
  return NULL;
}

int
main (int argc, char *argv[])
{
  long int i;
  long int child_count = 0;
  pthread_t *thread_id;
  union semun
  {
    int val;
    struct semid_ds *buf;
    ushort *array;
  } arg;

  if (argc != 2)
    {
      printf ("Usage: %s COUNT\n", argv[0]);
      exit (EXIT_FAILURE);
    }
  sscanf (argv[1], "%ld", &child_count);

  fprintf (stdout, "%ld Children are going to Jerusalem\n", child_count);

  thread_id = (pthread_t *) malloc (child_count * sizeof (pthread_t));

  create_semaphore1 ();
  create_barrier1 ();
  create_barrier2 ();
  activate_barrier1 ();
  activate_barrier2 ();

  for (i = 0; i < child_count; i++)
    {
      fprintf (stdout, "creating thread#: %d\n", i);
      pthread_create (&thread_id[i], NULL, &child, (void *) i);
    }

  for (i = child_count; i > 0; i--)
    {
      arg.val = 0;
      fprintf (stdout,
	       "teacher before release of %ld children at Barrier1 %d\n",
	       child_count, semctl (barrier1, 0, GETVAL, arg));
      release_barrier1 (child_count);	// now all children start rushing in
      // do the singing
      arg.val = 0;
      fprintf (stdout,
	       "teacher after release of %ld children at Barrier1 %d\n",
	       child_count, semctl (barrier1, 0, GETVAL, arg));

      // wait until all children finished
      // semaphore operation to wait before barrier2

      struct sembuf acquire2 = { 0, (-1) * child_count, SEM_UNDO };
      arg.val = 0;
      fprintf (stdout, "teacher waiting before barrier2 acquire: %d\n",
	       semctl (barrier2, 0, GETVAL, arg));
      if (semop (barrier2, &acquire2, 1) == -1)
	{
	  perror ("teacher: barrier2 -- acquire ");
	  exit (EXIT_FAILURE);
	}			// end acquire failure
      arg.val = 0;
      fprintf (stdout, "teacher after barrier2 acquire: %d\n",
	       semctl (barrier2, 0, GETVAL, arg));
      arg.val = 0;
      fprintf (stdout, "teacher before activating barrier1 : %d\n",
	       semctl (barrier1, 0, GETVAL, arg));
      activate_barrier1 ();
      arg.val = 0;
      fprintf (stdout, "teacher after barrier1 activation: %d\n",
	       semctl (barrier1, 0, GETVAL, arg));
    }

  // clean up
  destroy_semaphore1 (); // child semaphore has to be destroyed first for a clean end
  destroy_barrier2 ();
  destroy_barrier1 ();

  for (i = child_count - 1; i >= 0; i--)
    {
      fprintf (stdout, "ending  thread#: %d\n", i);
      pthread_kill (thread_id[i], SIGHUP);
    }

  return EXIT_SUCCESS;
}
