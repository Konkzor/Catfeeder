# -*- coding: UTF-8 -*-

from flask import Flask, render_template, request, jsonify
import datetime
import itertools
import copy
app = Flask(__name__)

@app.route("/")
@app.route("/accueil")
def hello():
	now = datetime.datetime.now()
	timeString = now.strftime("%Y-%m-%d %H:%M:%S")
	templateData ={
		'time' : timeString
		}
	return render_template('main.html', **templateData)

@app.route("/settings", methods=['GET', 'POST'])
def settings():
	templateData={
		'msgcode' : 0,
	}
	msgarray=[]
	# POST method
	if request.method == 'POST':
		# Get number of meals
		nbrepas = (int)(request.form['nbrepas'])
		templateData['nbrepas'] = nbrepas
		# Get all meals from the form
		repas=[]
		for i in range(1,nbrepas+1):
			repas.append([int(request.form["heure"+str(i)]),int(request.form["min"+str(i)]), int(request.form["tours"+str(i)])])
		# Sort all meal in time order (hour and minutes)
		repas.sort(key=lambda x: (x[0],x[1]))
		templateData['repas'] = repas
		# Search for 2 identical times
		repas_hm = copy.deepcopy(repas)
		for r in repas_hm:
			del r[2]
		if(len(list(repas_hm for repas_hm,_ in itertools.groupby(repas_hm)))!= len(repas)) :
			templateData['msgcode'] = 2 # fail
			msgarray.append('[ERREUR] Au moins 2 repas ont été programmés à la même heure !')
			templateData['msg'] = msgarray
			return render_template('formulaire.html', **templateData)

		# Everything is ok : save to file
		f = open("files/meals", 'w')
		f.write(str(nbrepas)+'\n')
		for i in range(0,nbrepas):
			f.write(str(repas[i][0])+':'+str(repas[i][1])+','+str(repas[i][2])+'\n')
		f.close()
		# Send msg to the client
		msg = "Nombre de repas : {nb} [".format(nb=str(nbrepas))
		for r in repas:
			msg+="{h}:{min}({nb}), ".format(h=str(r[0]), min=str(r[1]), nb=str(r[2]))
		templateData['msgcode'] = 1 # success
		msgarray.append('[SUCCES] '+msg[:-2]+']')
		templateData['msg'] = msgarray
		return render_template('formulaire.html', **templateData)
	
	# GET method
	templateData = getMeals()
	templateData['msgcode'] = 0 # info
	msgarray.append('Modifiez les paramètres puis cliquez sur "Mettre à jour"')
	templateData['msg'] = msgarray
	return render_template('formulaire.html', **templateData)

def getMeals():
	f = open("files/meals", 'r+')
	nbrepas =int(f.readline())
	templateData={
		'nbrepas' : nbrepas
	}
	repas=[]
	for i in range(1,nbrepas+1):
		line = f.readline()
		repas.append([(int)(line[:line.find(":")]), (int)(line[line.find(":")+1:line.find(",")]), (int)(line[line.find(",")+1:])])
	templateData['repas'] = repas
	f.close()
	return templateData

def getMealsForArduino():
	f = open("files/meals", 'r+')
	nbrepas = int(f.readline())
	templateData={
		'nb' : nbrepas
	}
	for i in range(1,nbrepas+1):
		line = f.readline()
		templateData['r'+str(i)] = line[:-1]
	f.close()
	return templateData

@app.route("/schedule")
def schedule():
	templateData = getMealsForArduino()
	return jsonify(**templateData)


@app.route("/time")
def time():
	now = datetime.datetime.now()
	Data ={
		's' : int(now.strftime("%S")),
		'mi' : int(now.strftime("%M")),
		'h' : int(now.strftime("%H"))+2,
		'j' : int(now.strftime("%d")),
		'js' : int(now.strftime("%w")),
		'mo' : int(now.strftime("%m")),
		'y' : int(now.strftime("%y"))
	}
	return jsonify(**Data)

@app.route("/logger")
def logger():
	f = open("files/log", 'r+')
	content = f.readlines()
	if(len(content) > 20) :
		maxlen = 20
	else :
		maxlen = len(content)
	logs=[]
	for i in range(0,maxlen):
		logs.append(content[i])

	templateData={
		'nblog' : maxlen,
		'logs' : logs
	}
 	f.close()
	return render_template('logger.html', **templateData)

if __name__ == "__main__":
	app.run()
